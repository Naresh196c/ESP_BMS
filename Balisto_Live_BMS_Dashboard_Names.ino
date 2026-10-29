// Balisto Live BMS Dashboard — ESP32-WROOM (classic ESP32 Arduino BLE library)
// BLE scans and battery reads run in a FreeRTOS worker; the website stays online.
// This firmware only sends the previously verified FF04 status request.

#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <vector>
#include <atomic>
#include <stdarg.h>
#include "bms_gatt.h"
#include "secrets.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Configure Wi-Fi in the local, Git-ignored secrets.h file.
const uint32_t SCAN_SECONDS = 10;
const uint32_t READ_TIMEOUT_MS = 6000;
const uint32_t CONNECT_TIMEOUT_MS = 15000;
const uint32_t DISCOVERY_TIMEOUT_MS = 20000;
const uint32_t GATT_OPERATION_TIMEOUT_MS = 5000;
const uint32_t CLEANUP_TIMEOUT_MS = 35000;
const char* FIRMWARE_VERSION = "BMS-2026-10-16-R7";
const uint32_t BETWEEN_SCANS_MS = 5UL * 60UL * 1000UL;
const uint32_t RETAIN_UNSEEN_MS = 30UL * 60UL * 1000UL;

static BLEUUID BMS_SERVICE((uint16_t)0xFFE0);
static BLEUUID BMS_CHARACTERISTIC((uint16_t)0xFFE1);
static BLEUUID BMS_AD_SERVICE((uint16_t)0xFEE7);
WebServer server(80);
SemaphoreHandle_t dataMutex;

struct BatteryState {
  String mac;
  String name;  // Full advertised name, e.g. SP16S60A2026050701549
  uint8_t addressType = 0;
  int rssi = -127;
  int soc = -1;
  float voltage = 0;
  uint16_t cycles = 0;
  bool hasReading = false;
  String status = "Discovered";
  uint32_t lastSeen = 0;
  uint32_t lastGood = 0;
  uint32_t attemptedScan = 0;
};

std::vector<BatteryState> batteries; // No arbitrary 10/20-device cap.
String workerStage = "Starting BLE...";
uint32_t scanNumber = 0;
uint32_t totalBLESeen = 0;
uint32_t candidatesSeen = 0;
uint32_t skippedAdvertisements = 0;
std::atomic<bool> blePaused(false);
std::atomic<bool> bleFaulted(false);
std::atomic<bool> bleCleanupPending(false);
uint32_t bleCleanupFailures = 0; // Protected by dataMutex.
BmsGatt bmsGatt;
enum class BmsWait { Open, Discovery, Search, Notify, Descriptor, Write, Frame, Idle };

int findBatteryLocked(const String& mac) {
  for (size_t i = 0; i < batteries.size(); i++) {
    if (batteries[i].mac == mac) return (int)i;
  }
  return -1;
}

void setStage(const String& stage) {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  workerStage = stage;
  xSemaphoreGive(dataMutex);
}

void updateBattery(const String& mac, const String& status,
                   bool newReading = false, int soc = -1,
                   float voltage = 0, uint16_t cycles = 0) {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  int i = findBatteryLocked(mac);
  if (i >= 0) {
    BatteryState& b = batteries[i];
    b.status = status;
    if (newReading) {
      b.soc = soc;
      b.voltage = voltage;
      b.cycles = cycles;
      b.hasReading = true;
      b.lastGood = millis();
    }
  }
  xSemaphoreGive(dataMutex);
}

bool waitForGatt(BmsWait event, uint32_t timeoutMs) {
  uint32_t started = millis();
  do {
    auto s = bmsGatt.snapshot();
    bool complete = false;
    switch (event) {
      case BmsWait::Open: complete = s.openDone; break;
      case BmsWait::Discovery: complete = s.discoveryDone; break;
      case BmsWait::Search: complete = s.searchDone; break;
      case BmsWait::Notify: complete = s.notifyDone; break;
      case BmsWait::Descriptor: complete = s.descriptorDone; break;
      case BmsWait::Write: complete = s.writeDone; break;
      case BmsWait::Frame: complete = s.rx.ready; break;
      case BmsWait::Idle: complete = s.link.idle(); break;
    }
    if (complete) return true;
    if (event != BmsWait::Open && event != BmsWait::Idle && !s.link.connected) return false;
    delay(20);
  } while (millis() - started < timeoutMs);
  return false;
}

bool gattOperationOK(const String& mac, const char* phase, BmsWait event,
                     uint32_t timeoutMs, esp_err_t submitted) {
  if (submitted != ESP_OK) {
    Serial.printf("  %s submission failed: %s (0x%04X)\n", phase,
                  esp_err_to_name(submitted), (unsigned)submitted);
    updateBattery(mac, String(phase) + " request failed");
    return false;
  }
  if (!waitForGatt(event, timeoutMs)) {
    auto s = bmsGatt.snapshot();
    Serial.printf("  %s %s after %lu ms; disconnect reason=0x%04X\n", phase,
                  s.link.connected ? "deadline reached" : "disconnected",
                  (unsigned long)timeoutMs, s.disconnectReason);
    updateBattery(mac, String(phase) + (s.link.connected ? " timeout" : " disconnected"));
    return false;
  }
  auto s = bmsGatt.snapshot();
  uint16_t status = 0xFFFF;
  switch (event) {
    case BmsWait::Discovery: status = s.discoveryStatus; break;
    case BmsWait::Search: status = s.searchStatus; break;
    case BmsWait::Notify: status = s.notifyStatus; break;
    case BmsWait::Descriptor: status = s.descriptorStatus; break;
    case BmsWait::Write: status = s.writeStatus; break;
    default: break;
  }
  if (status == ESP_GATT_OK) return true;
  Serial.printf("  %s failed: native GATT status=0x%04X\n", phase, status);
  updateBattery(mac, String(phase) + " failed");
  return false;
}

void finishBatteryConnection() {
  auto before = bmsGatt.snapshot();
  esp_err_t result = bmsGatt.close();
  if (result != ESP_OK)
    Serial.printf("  Disconnect request returned %s (0x%04X)\n", esp_err_to_name(result), (unsigned)result);
  if (before.link.opening)
    Serial.println("  Waiting for pending controller OPEN; the next battery cannot reuse this slot yet.");
  uint32_t timeout = CLEANUP_TIMEOUT_MS;
  if (!waitForGatt(BmsWait::Idle, timeout)) {
    bleFaulted.store(true);
    bleCleanupPending.store(true);
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    ++bleCleanupFailures;
    workerStage = "BLE waiting for controller cleanup; saved data available. Resumes after disconnect.";
    xSemaphoreGive(dataMutex);
    auto pending = bmsGatt.snapshot();
    Serial.printf("  BLE quarantined: OPEN pending=%s connected=%s CLOSE seen=%s CLOSE status=0x%04X. Waiting for terminal events; Wi-Fi/data remain available.\n",
                  pending.link.opening ? "yes" : "no", pending.link.connected ? "yes" : "no",
                  pending.closeSeen ? "yes" : "no", pending.closeStatus);
  } else {
    auto s = bmsGatt.snapshot();
    Serial.printf("  Connection released; disconnect reason=0x%04X\n", s.disconnectReason);
  }
}

void logStatusFailure(const FF04Receiver& rx) {
  Serial.printf("  Status response: %s; notifications=%lu bytes=%lu buffered=%u expected=%u rejected=%lu discarded=%lu last rejection=%s\n",
                rx.reason, (unsigned long)rx.notifications, (unsigned long)rx.bytes,
                (unsigned)rx.used, (unsigned)rx.expected, (unsigned long)rx.rejected,
                (unsigned long)rx.discarded, rx.lastReject);
  const uint8_t* data = rx.lastLength ? rx.lastFrame : rx.buffer;
  size_t length = rx.lastLength ? rx.lastLength : rx.used;
  if (length) {
    Serial.print("  RX hex (up to 64 bytes):");
    for (size_t i = 0; i < length && i < 64; ++i) Serial.printf(" %02X", data[i]);
    Serial.println();
  }
}

// All waits happen in the worker. Native callbacks only update bounded state.
void readBattery(const String& mac, uint8_t addressType, int rssi) {
  uint32_t started = millis();
  setStage("Reading " + mac);
  updateBattery(mac, "Connecting...");
  Serial.printf("Connecting: %s (type %u, RSSI %d dBm, OPEN wait %lu ms)\n",
                mac.c_str(), addressType, rssi, (unsigned long)CONNECT_TIMEOUT_MS);
  esp_err_t submitted = bmsGatt.open(BLEAddress(mac, addressType), addressType);
  if (submitted != ESP_OK || !waitForGatt(BmsWait::Open, CONNECT_TIMEOUT_MS)) {
    Serial.printf("  OPEN %s after %lu ms; submission=0x%04X\n",
                  submitted == ESP_OK ? "deadline reached" : "request failed",
                  (unsigned long)(millis() - started), (unsigned)submitted);
    updateBattery(mac, submitted == ESP_OK ? "Connection deadline reached" : "Connection request failed");
    finishBatteryConnection();
    return;
  }
  auto opened = bmsGatt.snapshot();
  if (opened.openStatus != ESP_GATT_OK) {
    Serial.printf("  Connection failed after %lu ms; native OPEN status=0x%04X\n",
                  (unsigned long)(millis() - started), opened.openStatus);
    updateBattery(mac, "Connection failed");
    finishBatteryConnection();
    return;
  }
  // Return from any failed phase to the shared disconnect/cleanup path.
  [&]() {
    Serial.printf("  Connected at %lu ms; discovering FFE0...\n", (unsigned long)(millis() - started));
    updateBattery(mac, "Discovering services...");
    if (!gattOperationOK(mac, "Discovery", BmsWait::Discovery, DISCOVERY_TIMEOUT_MS, ESP_OK)) return;
    if (!gattOperationOK(mac, "Service search", BmsWait::Search, GATT_OPERATION_TIMEOUT_MS, bmsGatt.search())) return;
    auto service = bmsGatt.snapshot();
    if (!service.serviceStart || !service.serviceEnd) {
      updateBattery(mac, "FFE0 service missing"); Serial.println("  FFE0 service missing"); return;
    }
    esp_gatt_status_t found = bmsGatt.findCharacteristic();
    if (found != ESP_GATT_OK) {
      updateBattery(mac, "FFE1 characteristic unavailable");
      Serial.printf("  FFE1 lookup/properties failed: 0x%04X\n", (unsigned)found); return;
    }
    updateBattery(mac, "Subscribing...");
    if (!gattOperationOK(mac, "Notify registration", BmsWait::Notify, GATT_OPERATION_TIMEOUT_MS, bmsGatt.registerNotifications())) return;
    if (!gattOperationOK(mac, "Notify descriptor", BmsWait::Descriptor, GATT_OPERATION_TIMEOUT_MS, bmsGatt.enableNotifications())) return;
    delay(250); // Preserve the verified BMS subscription-settling interval.
    updateBattery(mac, "Requesting status...");
    if (!gattOperationOK(mac, "Status write", BmsWait::Write, GATT_OPERATION_TIMEOUT_MS, bmsGatt.requestStatus())) return;
    Serial.printf("  Status request acknowledged at %lu ms; awaiting FF04...\n", (unsigned long)(millis() - started));
    updateBattery(mac, "Waiting for status...");
    bool received = waitForGatt(BmsWait::Frame, READ_TIMEOUT_MS);
    bmsGatt.stopReceiver();
    auto response = bmsGatt.snapshot();
    if (received && response.rx.ready) {
      updateBattery(mac, "OK", true, response.rx.soc, response.rx.voltage, response.rx.cycles);
      Serial.printf("  SOC: %d%%  Voltage: %.2f V  Cycles: %u\n", response.rx.soc,
                    response.rx.voltage, (unsigned)response.rx.cycles);
    } else {
      updateBattery(mac, response.rx.rejected ? "Invalid status frame" : "Status response timeout");
      Serial.printf("  FF04 wait %s; disconnect reason=0x%04X\n",
                    response.link.connected ? "expired" : "lost connection", response.disconnectReason);
      logStatusFailure(response.rx);
    }
  }();
  finishBatteryConnection();
  Serial.printf("  Read finished in %lu ms\n", (unsigned long)(millis() - started));
}

// With duplicate callbacks enabled, classic Arduino BLE releases each
// advertisement after this callback instead of retaining every nearby device.
// Only matching BMS information is retained in our mutex-protected list.
class BMSScanCallbacks : public BLEAdvertisedDeviceCallbacks {
 public:
  void onResult(BLEAdvertisedDevice d) override {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    ++totalBLESeen; // Advertisement reports, including repeats; not unique devices.
    xSemaphoreGive(dataMutex);
    String mac = String(d.getAddress().toString().c_str());
    mac.toLowerCase();
    String name = String(d.getName().c_str());
    name.trim();
    name.toUpperCase();

    // A BLE advertisement may omit its name. Only keep device names
    // that match our known Super Power BMS naming pattern. Restrict
    // to alphanumeric characters so names are safe to embed in JSON.
    String displayName = "";
    if (name.startsWith("SP16S60A") && name.length() <= 40) {
      bool safe = true;
      for (size_t j = 0; j < name.length(); j++) {
        char ch = name[j];
        if (!((ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9'))) {
          safe = false;
          break;
        }
      }
      if (safe) displayName = name;
    }

    // Same practical BMS identification rules as the successful scan.
    bool knownMAC = mac.startsWith("12:34:5b:");
    bool knownName = name.startsWith("SP16S60A");
    bool knownServices = d.isAdvertisingService(BMS_SERVICE) &&
                         d.isAdvertisingService(BMS_AD_SERVICE);
    if (!knownMAC && !knownName && !knownServices) return;

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    int idx = findBatteryLocked(mac);
    if (idx < 0) {
      // Keep headroom for the web server and BLE connection allocations.
      if (batteries.size() == batteries.capacity()) {
        size_t capacity = batteries.capacity() + 4;
        size_t bytes = capacity * sizeof(BatteryState);
        if (ESP.getFreeHeap() < bytes + 24000 || ESP.getMaxAllocHeap() < bytes + 1024) {
          ++skippedAdvertisements;
          xSemaphoreGive(dataMutex);
          return;
        }
        batteries.reserve(capacity);
      }
      BatteryState b;
      b.mac = mac;
      b.name = displayName;
      b.addressType = d.getAddressType();
      b.rssi = d.getRSSI();
      b.lastSeen = millis();
      batteries.push_back(b);
    } else {
      // Preserve an earlier known name when this scan omits the name.
      if (displayName.length() > 0) batteries[idx].name = displayName;
      batteries[idx].rssi = d.getRSSI();
      batteries[idx].addressType = d.getAddressType();
      batteries[idx].lastSeen = millis();
    }
    xSemaphoreGive(dataMutex);
  }
};
BMSScanCallbacks scanCallbacks;

void scanAndRead(BLEScan* scanner) {
  // Keep this fleet's scan token across pause/quarantine so a late
  // disconnect resumes remaining devices instead of restarting at the top.
  static bool fleetActive = false;
  static uint32_t started = 0;
  if (!fleetActive) {
    setStage("Scanning nearby batteries...");
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    totalBLESeen = 0;
    skippedAdvertisements = 0;
    started = millis();
    for (size_t i = 0; i < batteries.size();) {
      if (started - batteries[i].lastSeen > RETAIN_UNSEEN_MS)
        batteries.erase(batteries.begin() + i);
      else ++i;
    }
    xSemaphoreGive(dataMutex);
    Serial.println("\nScanning nearby batteries for 10 seconds (filtered streaming scan)...");
    scanner->start(SCAN_SECONDS, false);
    scanner->clearResults();

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    scanNumber++;
    candidatesSeen = 0;
    for (const BatteryState& battery : batteries)
      if (int32_t(battery.lastSeen - started) >= 0) ++candidatesSeen;
    Serial.printf("Scan complete: %lu advertisement reports, %lu BMS candidates, free heap: %lu, largest block: %lu, skipped: %lu\n",
                  (unsigned long)totalBLESeen, (unsigned long)candidatesSeen,
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
                  (unsigned long)skippedAdvertisements);
    xSemaphoreGive(dataMutex);
    fleetActive = true;
  }
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  size_t count = batteries.size();
  xSemaphoreGive(dataMutex);

  // No second candidate list and no copying complete BLE scan results.
  // The callback only runs during scans; all shared accesses still use the mutex.
  for (size_t attempt = 0; attempt < count && !blePaused.load() && !bleFaulted.load(); ++attempt) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    int best = -1;
    for (size_t i = 0; i < batteries.size(); ++i) {
      if (int32_t(batteries[i].lastSeen - started) < 0 ||
          batteries[i].attemptedScan == scanNumber) continue;
      if (best < 0 || batteries[i].rssi > batteries[best].rssi) best = i;
    }
    if (best < 0) {
      xSemaphoreGive(dataMutex);
      break;
    }
    batteries[best].attemptedScan = scanNumber;
    String mac = batteries[best].mac;
    uint8_t addressType = batteries[best].addressType;
    int rssi = batteries[best].rssi;
    xSemaphoreGive(dataMutex);
    readBattery(mac, addressType, rssi);
    if (bleFaulted.load()) return;
    delay(250); // Allow Wi-Fi airtime between BLE reads.
  }
  if (blePaused.load()) {
    setStage("BLE paused; Wi-Fi diagnostic mode");
    return;
  }
  fleetActive = false;
  setStage("Waiting for the next scan...");
}

void bluetoothWorker(void* parameter) {
  BLEDevice::init("Balisto-Monitor");
  BLEScan* scanner = BLEDevice::getScan();
  scanner->setAdvertisedDeviceCallbacks(&scanCallbacks, true);
  scanner->setActiveScan(true);
  scanner->setInterval(100);
  scanner->setWindow(30);
  esp_err_t registered = bmsGatt.begin();
  uint32_t registrationStarted = millis();
  while (registered == ESP_OK && !bmsGatt.snapshot().registrationDone &&
         millis() - registrationStarted < GATT_OPERATION_TIMEOUT_MS) delay(20);
  auto registration = bmsGatt.snapshot();
  if (registered != ESP_OK || !registration.registrationDone || registration.registrationStatus != ESP_GATT_OK) {
    bleFaulted.store(true);
    setStage("BLE client registration failed; saved data available. Reset required.");
    Serial.printf("GATT registration failed: submit=0x%04X status=0x%04X\n", (unsigned)registered, registration.registrationStatus);
  } else Serial.println("Persistent native GATT client registered; no per-battery stack restart.");
  for (;;) {
    if (bleFaulted.load()) {
      // Only cleanup quarantine can clear automatically; a failed client
      // registration still requires reset. Never force an occupied slot idle.
      if (bleCleanupPending.load() && bmsGatt.snapshot().link.idle()) {
        bleCleanupPending.store(false);
        bleFaulted.store(false);
        setStage("Late controller cleanup confirmed; resuming remaining batteries...");
        Serial.println("Late controller cleanup confirmed; resuming remaining batteries.");
      } else { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    }
    if (blePaused.load()) {
      setStage("BLE paused; Wi-Fi diagnostic mode");
      vTaskDelay(pdMS_TO_TICKS(100)); continue;
    }
    uint32_t cycleStarted = millis();
    scanAndRead(scanner);
    while (millis() - cycleStarted < BETWEEN_SCANS_MS && !blePaused.load() && !bleFaulted.load())
      vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Website is hosted wholly inside ESP32: no CDN, internet, or phone app.
const char PAGE[] PROGMEM = R"PAGE(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Balisto Live Battery Monitor</title>
<style>
:root{color-scheme:dark;font-family:Arial,sans-serif}
*{box-sizing:border-box}body{margin:0;background:#0b1220;color:#f1f5f9;padding:22px}
main{max-width:1130px;margin:auto}header{display:flex;gap:14px;align-items:center;justify-content:space-between;flex-wrap:wrap}
h1{font-size:30px;color:#35d399;margin:0}small,.muted{color:#9fb0c4}h2{font-size:17px;margin:3px 0 0;color:#cbd5e1;font-weight:400}
.pill{padding:9px 13px;border:1px solid #365267;border-radius:100px;color:#35d399}
.stats{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px;margin:22px 0}
.stat,.card{background:#1d2a3d;border:1px solid #283b51;border-radius:14px;padding:16px}
.stat strong{display:block;font-size:28px;margin-top:6px}.stage{color:#a8bbd2;margin:0 0 15px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(235px,1fr));gap:14px}
.card h3{font-size:14px;margin:0 0 8px;overflow-wrap:anywhere;word-break:break-word}.mac{font-size:12px;color:#a5b7ce;overflow-wrap:anywhere}
.soc{font-size:36px;font-weight:bold;margin:14px 0 9px}.track{height:9px;background:#354459;border-radius:10px;overflow:hidden}
.fill{height:100%;background:#35d399;border-radius:10px}.info{display:flex;justify-content:space-between;font-size:13px;margin-top:12px}
.status{font-size:12px;color:#aabace;margin-top:9px;min-height:28px}.bad{color:#fb7185}.warn{color:#fbbf24}
footer{color:#9fb0c4;font-size:12px;margin-top:26px;line-height:1.6}
@media(max-width:450px){body{padding:12px}.stat{padding:10px}.stat strong{font-size:24px}}
</style></head><body><main>
<header><div><h1>BALISTO</h1><h2>Live Battery Monitoring System</h2></div>
<div class="pill" id="connection">Connecting...</div></header>
<section class="stats">
<div class="stat"><small>Discovered</small><strong id="discovered">0</strong></div>
<div class="stat"><small>Readings</small><strong id="successful">0</strong></div>
<div class="stat"><small>Unavailable</small><strong id="unavailable">0</strong></div>
</section>
<p class="muted">Firmware BMS-2026-10-16-R7 &bull; Refreshes every 5 minutes &bull; <button id="refresh" type="button">Refresh now</button> &bull; <a href="/health" style="color:#35d399">Connection diagnostics</a> &bull; <button id="pause" type="button">Pause Bluetooth</button> <span id="control"></span></p>
<p class="stage" id="stage">Waiting for first BLE scan...</p>
<section class="grid" id="list"></section>
<footer>Read-only BMS information • Data refreshes automatically • Each battery is read in sequence.
Data may be delayed when Bluetooth signal is weak. Don't use this page for safety-critical decisions.</footer>
</main><script>
const byId=id=>document.getElementById(id);
const EXPECTED_FIRMWARE='BMS-2026-10-16-R7';
let paused=false,refreshInFlight=false,nextRefreshTimer;
byId('refresh').onclick=()=>refresh();
byId('pause').onclick=async()=>{
 try{
  const response=await fetch(paused?'/api/resume':'/api/pause',{method:'POST'});
  const message=await response.text();
  if(!response.ok)throw new Error(message||'HTTP '+response.status);
  byId('control').textContent=message;
  paused=!paused;byId('pause').textContent=paused?'Resume Bluetooth':'Pause Bluetooth';
 }catch(err){byId('control').textContent=err.message}
};
function div(cls,txt){const e=document.createElement('div');e.className=cls;if(txt!==undefined)e.textContent=txt;return e}
function card(b){
 const el=div('card'), name=document.createElement('h3');
 name.textContent=b.name||'Unnamed BMS';el.append(name,div('mac','MAC: '+b.mac));
 if(b.hasReading){
   const soc=div('soc',b.soc+'%');if(b.stale)soc.classList.add('warn');el.append(soc);
   const bar=div('track'),fill=div('fill');fill.style.width=b.soc+'%';
   fill.style.background=b.soc<=20?'#fb7185':b.soc<=40?'#fbbf24':'#35d399';bar.append(fill);el.append(bar);
   el.append(div('info','Voltage: '+b.voltage.toFixed(2)+' V'));
   el.append(div('info','Cycles: '+b.cycles+'  |  RSSI: '+b.rssi+' dBm'));
   el.append(div('status',(b.stale?'STALE • ':'')+b.status+' • last reading '+b.readAgeSec+'s ago'));
 } else {
   el.append(div('soc bad','Unavailable'));
   el.append(div('status',b.status+' • RSSI: '+b.rssi+' dBm'));
 }
 el.append(div('status','Seen '+b.seenAgeSec+'s ago'));return el;
}
async function getPage(offset){
 const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),10000);
 try{
  const response=await fetch('/api/batteries?offset='+offset,{cache:'no-store',signal:controller.signal});
  const body=await response.text();
  if(!response.ok)throw new Error('HTTP '+response.status+': '+body.slice(0,100));
  let data;
  try{data=JSON.parse(body)}catch(err){throw new Error('Invalid JSON ('+body.length+' bytes): '+body.slice(0,60))}
  if(data.firmware!==EXPECTED_FIRMWARE)throw new Error('Firmware mismatch. Reload this page after uploading R7.');
  if(!Array.isArray(data.batteries))throw new Error('API response is missing battery data');
  return data;
 }finally{clearTimeout(timer)}
}
function render(data,received){
 const rows=[...received.values()];
 byId('connection').textContent='ESP32 ONLINE';
 byId('discovered').textContent=data.total;
 byId('successful').textContent=rows.filter(b=>b.hasReading&&!b.stale).length;
 byId('unavailable').textContent=rows.filter(b=>!b.hasReading||b.stale).length;
 byId('stage').textContent=data.stage+' | Scan cycle '+data.scanNumber+
  ' | Loaded '+rows.length+'/'+data.total+' batteries';
 byId('list').replaceChildren(...rows.map(card));
}
async function refresh(){
 if(refreshInFlight)return;
 refreshInFlight=true;
 clearTimeout(nextRefreshTimer);
 byId('refresh').disabled=true;
 const received=new Map();
 try{
  let offset=0;
  do{
   const data=await getPage(offset);
   for(const b of data.batteries)received.set(b.mac,b);
   render(data,received); // Show every page immediately; don't wait for the fleet.
   const next=data.next;
   if(next!==-1&&(!Number.isInteger(next)||next<=offset))throw new Error('Invalid API page sequence');
   offset=next;
  }while(offset!==-1);
 }catch(err){
  byId('connection').textContent=received.size?'PARTIAL DATA; RETRYING':'DATA REQUEST FAILED';
  byId('stage').textContent=err.name==='AbortError'?'10-second network timeout; check Wi-Fi and /health':err.message;
 }finally{
  refreshInFlight=false;byId('refresh').disabled=false;
  nextRefreshTimer=setTimeout(refresh,300000);
 }
}
refresh();
</script></body></html>
)PAGE";

// Bounded pages avoid a large allocation and a long chunked response.
// This buffer belongs only to the HTTP task. BLE never touches it.
static char apiBuffer[4096];
static size_t apiUsed = 0;
static bool apiOverflow = false;
void apiAppend(const char* format, ...) {
  if (apiOverflow) return;
  va_list args;
  va_start(args, format);
  int n = vsnprintf(apiBuffer + apiUsed, sizeof(apiBuffer) - apiUsed, format, args);
  va_end(args);
  if (n < 0 || size_t(n) >= sizeof(apiBuffer) - apiUsed) apiOverflow = true;
  else apiUsed += size_t(n);
}

void handleAPI() {
  uint32_t requestStarted = millis();
  int requested = server.hasArg("offset") ? server.arg("offset").toInt() : 0;
  size_t offset = requested > 0 ? size_t(requested) : 0;
  apiUsed = 0;
  apiOverflow = false;
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  size_t total = batteries.size();
  if (offset > total) offset = total;
  size_t end = offset + 8;
  if (end > total) end = total;
  // Staleness must account for the time needed to visit a large fleet.
  uint32_t staleMs = 2 * BETWEEN_SCANS_MS;
  uint32_t fleetAllowance = (uint32_t)total * 12000UL + 30000UL;
  if (fleetAllowance > staleMs) staleMs = fleetAllowance;
  apiAppend("{\"firmware\":\"%s\",\"stage\":\"%s\",\"scanNumber\":%lu,"
            "\"totalBLE\":%lu,\"candidates\":%lu,\"total\":%u,\"offset\":%u,"
            "\"next\":%d,\"paused\":%s,\"batteries\":[",
            FIRMWARE_VERSION, workerStage.c_str(), (unsigned long)scanNumber,
            (unsigned long)totalBLESeen, (unsigned long)candidatesSeen,
            (unsigned)total, (unsigned)offset, end < total ? int(end) : -1,
            blePaused.load() ? "true" : "false");
  for (size_t i = offset; i < end; ++i) {
    const BatteryState& b = batteries[i];
    uint32_t now = millis();
    bool stale = !b.hasReading || now - b.lastGood > staleMs;
    apiAppend("%s{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"soc\":%d,"
              "\"voltage\":%.2f,\"cycles\":%u,\"hasReading\":%s,\"stale\":%s,"
              "\"status\":\"%s\",\"seenAgeSec\":%lu,\"readAgeSec\":%lu}",
              i == offset ? "" : ",", b.mac.c_str(), b.name.c_str(), b.rssi, b.soc,
              (double)b.voltage, (unsigned)b.cycles, b.hasReading ? "true" : "false",
              stale ? "true" : "false", b.status.c_str(),
              (unsigned long)((now - b.lastSeen) / 1000UL),
              (unsigned long)(b.hasReading ? (now - b.lastGood) / 1000UL : 0));
  }
  apiAppend("]}");
  xSemaphoreGive(dataMutex);
  if (apiOverflow) {
    server.send(503, "text/plain", "API page exceeded buffer");
    return;
  }
  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.setContentLength(apiUsed);
  server.send(200, "application/json; charset=utf-8", "");
  server.sendContent(apiBuffer, apiUsed);
  Serial.printf("HTTP page offset=%u count=%u total=%u bytes=%u duration=%lu ms\n",
                (unsigned)offset, (unsigned)(end - offset), (unsigned)total,
                (unsigned)apiUsed, (unsigned long)(millis() - requestStarted));
}

void handleHealth() {
  char health[768];
  auto link = bmsGatt.snapshot();
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  snprintf(health, sizeof(health),
           "firmware=%s\nboard=%s\nuptime_sec=%lu\nfree_heap=%lu\nlargest_block=%lu\n"
           "wifi_clients=%u\nbatteries=%u\nble_cleanup_failures=%lu\nble_pause_requested=%s\nble_faulted=%s\n"
           "ble_cleanup_pending=%s\nble_open_pending=%s\nble_connected=%s\nble_close_seen=%s\nble_close_status=0x%04X\nstage=%s\n",
           FIRMWARE_VERSION, ARDUINO_BOARD, (unsigned long)(millis() / 1000UL),
           (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
           (unsigned)WiFi.softAPgetStationNum(), (unsigned)batteries.size(), (unsigned long)bleCleanupFailures,
           blePaused.load() ? "yes" : "no", bleFaulted.load() ? "yes" : "no",
           bleCleanupPending.load() ? "yes" : "no", link.link.opening ? "yes" : "no",
           link.link.connected ? "yes" : "no", link.closeSeen ? "yes" : "no", link.closeStatus, workerStage.c_str());
  xSemaphoreGive(dataMutex);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", health);
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.printf("\nBALISTO LIVE BATTERY DASHBOARD %s\n", FIRMWARE_VERSION);
  Serial.printf("Selected Arduino board: %s\n", ARDUINO_BOARD);
#ifdef BOARD_HAS_DUAL_ANTENNA
  Serial.println("WROOM-DA dual-antenna profile enabled. Verify this matches the physical module.");
#endif
  dataMutex = xSemaphoreCreateMutex();
  if (!dataMutex) { Serial.println("Mutex allocation failed"); return; }

  // Wi-Fi becomes available BEFORE Bluetooth scanning starts.
  bool routerConnected = false;
  if (ROUTER_SSID[0]) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ROUTER_SSID, ROUTER_PASS);
    Serial.println("Connecting to configured router...");
    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 20000UL) delay(100);
    routerConnected = WiFi.status() == WL_CONNECTED;
    if (!routerConnected) Serial.println("Router unavailable; falling back to Balisto-BMS hotspot.");
  }
  if (!routerConnected) {
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(AP_NAME, AP_PASS)) {
      Serial.println("Wi-Fi AP startup failed");
      return;
    }
  }
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.send_P(200, "text/html; charset=utf-8", PAGE);
  });
  server.on("/api/batteries", HTTP_GET, handleAPI);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/api/pause", HTTP_POST, []() {
    blePaused.store(true);
    server.send(200, "text/plain", "Pause requested; current BLE operation must finish first.");
  });
  server.on("/api/resume", HTTP_POST, []() {
    if (bleFaulted.load()) {
      if (bleCleanupPending.load()) {
        blePaused.store(false);
        server.send(202, "text/plain", "Waiting for controller cleanup; polling resumes automatically once disconnected.");
      } else server.send(409, "text/plain", "BLE client initialization failed; reset the board to resume.");
      return;
    }
    blePaused.store(false);
    server.send(200, "text/plain", "BLE resumed.");
  });
  server.on("/favicon.ico", HTTP_GET, []() { server.send(204); });
  server.begin();
  Serial.print("Wi-Fi: "); Serial.println(routerConnected ? ROUTER_SSID : AP_NAME);
  Serial.print("Website: http://"); Serial.println(routerConnected ? WiFi.localIP() : WiFi.softAPIP());
  Serial.println("Wi-Fi online. Starting background BLE scanning...");
  Serial.println("Serial commands: P = pause BLE after current operation, R = resume BLE.");

  BaseType_t taskOK = xTaskCreatePinnedToCore(bluetoothWorker,
                         "BMS-Worker", 16384, nullptr, 1, nullptr, 1);
  if (taskOK != pdPASS) {
    setStage("ERROR: Could not create BLE worker");
    Serial.println("BLE worker allocation failed");
  }
}

void loop() {
  if (Serial.available()) {
    char command = Serial.read();
    if (command == 'p' || command == 'P') {
      blePaused.store(true);
      Serial.println("BLE pause requested. Wait for the current operation to finish.");
    } else if (command == 'r' || command == 'R') {
      blePaused.store(false);
      Serial.println(bleFaulted.load() ? "BLE waiting for controller state; late cleanup resumes automatically. Initialization faults require reset." : "BLE resumed.");
    }
  }
  server.handleClient();
  delay(2);
}
