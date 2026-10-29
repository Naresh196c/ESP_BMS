#pragma once
#include <Arduino.h>
#include <BLEDevice.h>
#include <esp_gattc_api.h>
#include <esp_gap_ble_api.h>
#include "bms_protocol.h"

#if !defined(CONFIG_BLUEDROID_ENABLED)
#error "This firmware requires the classic ESP32 Bluedroid BLE stack."
#endif

struct BmsGattState {
  esp_gatt_if_t interface = ESP_GATT_IF_NONE;
  bool registrationDone = false;
  uint16_t registrationStatus = 0xFFFF;
  BmsLinkLifecycle link;
  uint8_t peer[6] = {};
  bool openDone = false, discoveryDone = false, searchDone = false;
  bool notifyDone = false, descriptorDone = false, writeDone = false;
  bool rxEnabled = false, disconnectIssued = false;
  uint16_t openStatus = 0xFFFF, discoveryStatus = 0xFFFF, searchStatus = 0xFFFF;
  uint16_t notifyStatus = 0xFFFF, descriptorStatus = 0xFFFF, writeStatus = 0xFFFF;
  uint16_t disconnectReason = 0, serviceStart = 0, serviceEnd = 0;
  bool closeSeen = false;
  uint16_t closeStatus = 0xFFFF;
  uint16_t characteristic = 0, descriptor = 0;
  FF04Receiver rx;
};

// One lifetime-long GATT registration. No client deletion, stack shutdown,
// or application unregister occurs while a controller operation is pending.
class BmsGatt {
  portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
  BmsGattState state;
  uint16_t appId = 0;
  static BmsGatt*& activeInstance() { static BmsGatt* instance = nullptr; return instance; }
  static void callback(esp_gattc_cb_event_t event, esp_gatt_if_t interface,
                       esp_ble_gattc_cb_param_t* p) {
    BmsGatt* instance = activeInstance();
    if (instance) instance->onEvent(event, interface, p);
  }
  void onEvent(esp_gattc_cb_event_t event, esp_gatt_if_t interface,
               esp_ble_gattc_cb_param_t* p) {
    bool closeLateConnection = false;
    bool requestMtu = false;
    uint16_t mtuConnection = 0xFFFF;
    uint8_t closeAddress[6] = {};
    portENTER_CRITICAL(&mutex);
    if (event == ESP_GATTC_REG_EVT && p->reg.app_id == appId) {
      state.registrationDone = true;
      state.registrationStatus = p->reg.status;
      if (p->reg.status == ESP_GATT_OK) state.interface = interface;
    } else if (state.interface != ESP_GATT_IF_NONE && interface == state.interface) {
      switch (event) {
        case ESP_GATTC_CONNECT_EVT:
          if (memcmp(p->connect.remote_bda, state.peer, 6) == 0 && state.link.opening) {
            state.link.onConnected(p->connect.conn_id);
            requestMtu = !state.link.abandoned;
            mtuConnection = p->connect.conn_id;
          }
          break;
        case ESP_GATTC_OPEN_EVT:
          if (memcmp(p->open.remote_bda, state.peer, 6) == 0 && state.link.opening) {
            state.openDone = true; state.openStatus = p->open.status;
            state.link.onOpen(p->open.status == ESP_GATT_OK, p->open.conn_id);
            if (p->open.status != ESP_GATT_OK && state.link.connected) state.link.abort();
          }
          break;
        case ESP_GATTC_DISCONNECT_EVT:
          if (memcmp(p->disconnect.remote_bda, state.peer, 6) == 0) {
            state.link.onDisconnected(); state.rxEnabled = false;
            state.disconnectReason = p->disconnect.reason;
          }
          break;
        case ESP_GATTC_CLOSE_EVT:
          if (memcmp(p->close.remote_bda, state.peer, 6) == 0) {
            state.closeSeen = true; state.closeStatus = p->close.status;
          }
          // A virtual GATT close alone does not confirm physical release.
          break;
        case ESP_GATTC_DIS_SRVC_CMPL_EVT:
          if (p->dis_srvc_cmpl.conn_id == state.link.connection) {
            state.discoveryDone = true; state.discoveryStatus = p->dis_srvc_cmpl.status;
          }
          break;
        case ESP_GATTC_SEARCH_RES_EVT:
          if (p->search_res.conn_id == state.link.connection &&
              p->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
              p->search_res.srvc_id.uuid.uuid.uuid16 == 0xFFE0) {
            state.serviceStart = p->search_res.start_handle;
            state.serviceEnd = p->search_res.end_handle;
          }
          break;
        case ESP_GATTC_SEARCH_CMPL_EVT:
          if (p->search_cmpl.conn_id == state.link.connection) {
            state.searchDone = true; state.searchStatus = p->search_cmpl.status;
          }
          break;
        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
          if (state.link.connected && !state.link.abandoned &&
              p->reg_for_notify.handle == state.characteristic) {
            state.notifyDone = true; state.notifyStatus = p->reg_for_notify.status;
          }
          break;
        case ESP_GATTC_WRITE_DESCR_EVT:
          if (p->write.conn_id == state.link.connection && p->write.handle == state.descriptor) {
            state.descriptorDone = true; state.descriptorStatus = p->write.status;
          }
          break;
        case ESP_GATTC_WRITE_CHAR_EVT:
          if (p->write.conn_id == state.link.connection && p->write.handle == state.characteristic) {
            state.writeDone = true; state.writeStatus = p->write.status;
          }
          break;
        case ESP_GATTC_NOTIFY_EVT:
          if (state.rxEnabled && !state.link.abandoned &&
              p->notify.conn_id == state.link.connection &&
              p->notify.handle == state.characteristic &&
              memcmp(p->notify.remote_bda, state.peer, 6) == 0)
            state.rx.feed(p->notify.value, p->notify.value_len);
          break;
        default: break;
      }
      if (state.link.abandoned && state.link.connected && !state.disconnectIssued) {
        state.disconnectIssued = true;
        memcpy(closeAddress, state.peer, 6); closeLateConnection = true;
      }
    }
    portEXIT_CRITICAL(&mutex);
    // Stack calls and logging never execute inside the callback's critical section.
    if (closeLateConnection) esp_ble_gap_disconnect(closeAddress);
    else if (requestMtu) esp_ble_gattc_send_mtu_req(interface, mtuConnection);
  }
 public:
  BmsGattState snapshot() {
    portENTER_CRITICAL(&mutex);
    BmsGattState copy = state;
    portEXIT_CRITICAL(&mutex);
    return copy;
  }
  esp_err_t begin() {
    activeInstance() = this;
    // The sketch owns exactly one native client; no BLEClient objects or
    // GATT servers are created. Keep its ID stable for the entire boot.
    appId = 0x424D;
    BLEDevice::setCustomGattcHandler(callback);
    return esp_ble_gattc_app_register(appId);
  }
  esp_err_t open(const BLEAddress& address, uint8_t type) {
    BmsGattState next;
    BmsGattState previous = snapshot();
    if (!previous.link.idle() || previous.interface == ESP_GATT_IF_NONE) return ESP_ERR_INVALID_STATE;
    next.interface = previous.interface;
    next.registrationDone = previous.registrationDone;
    next.registrationStatus = previous.registrationStatus;
    next.link.begin();
    BLEAddress copy = address;
    memcpy(next.peer, copy.getNative(), 6);
    portENTER_CRITICAL(&mutex); state = next; portEXIT_CRITICAL(&mutex);
    esp_err_t result = esp_ble_gattc_open(next.interface, next.peer, (esp_ble_addr_type_t)type, true);
    if (result != ESP_OK) {
      portENTER_CRITICAL(&mutex);
      state.link.onOpen(false, 0xFFFF); state.openDone = true; state.openStatus = result;
      portEXIT_CRITICAL(&mutex);
    }
    return result;
  }
  esp_err_t search() {
    auto s = snapshot();
    esp_bt_uuid_t uuid = {}; uuid.len = ESP_UUID_LEN_16; uuid.uuid.uuid16 = 0xFFE0;
    return esp_ble_gattc_search_service(s.interface, s.link.connection, &uuid);
  }
  esp_gatt_status_t findCharacteristic() {
    auto s = snapshot();
    esp_bt_uuid_t uuid = {}; uuid.len = ESP_UUID_LEN_16; uuid.uuid.uuid16 = 0xFFE1;
    esp_gattc_char_elem_t item = {}; uint16_t count = 1;
    esp_gatt_status_t status = esp_ble_gattc_get_char_by_uuid(s.interface, s.link.connection,
                                  s.serviceStart, s.serviceEnd, uuid, &item, &count);
    if (status != ESP_GATT_OK) return status;
    if (!count || !(item.properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) ||
        !(item.properties & ESP_GATT_CHAR_PROP_BIT_WRITE)) return ESP_GATT_NOT_FOUND;
    portENTER_CRITICAL(&mutex); state.characteristic = item.char_handle; portEXIT_CRITICAL(&mutex);
    return ESP_GATT_OK;
  }
  esp_err_t registerNotifications() {
    auto s = snapshot();
    return esp_ble_gattc_register_for_notify(s.interface, s.peer, s.characteristic);
  }
  esp_err_t enableNotifications() {
    auto s = snapshot();
    esp_bt_uuid_t uuid = {}; uuid.len = ESP_UUID_LEN_16; uuid.uuid.uuid16 = 0x2902;
    esp_gattc_descr_elem_t item = {}; uint16_t count = 1;
    esp_gatt_status_t status = esp_ble_gattc_get_descr_by_char_handle(s.interface,
                                      s.link.connection, s.characteristic, uuid, &item, &count);
    if (status != ESP_GATT_OK || !count) return ESP_FAIL;
    portENTER_CRITICAL(&mutex); state.descriptor = item.handle; portEXIT_CRITICAL(&mutex);
    uint8_t enabled[] = {1, 0};
    return esp_ble_gattc_write_char_descr(s.interface, s.link.connection, item.handle,
                      sizeof(enabled), enabled, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  }
  esp_err_t requestStatus() {
    portENTER_CRITICAL(&mutex); state.rx.reset(); state.rxEnabled = true; portEXIT_CRITICAL(&mutex);
    auto s = snapshot();
    uint8_t request[] = {0xEA, 0xD1, 0x01, 0x04, 0xFF, 0x04, 0xFF, 0xF5};
    return esp_ble_gattc_write_char(s.interface, s.link.connection, s.characteristic,
                sizeof(request), request, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  }
  void stopReceiver() {
    portENTER_CRITICAL(&mutex); state.rxEnabled = false; portEXIT_CRITICAL(&mutex);
  }
  esp_err_t close() {
    auto s = snapshot();
    if (s.notifyDone && s.notifyStatus == ESP_GATT_OK)
      esp_ble_gattc_unregister_for_notify(s.interface, s.peer, s.characteristic);
    portENTER_CRITICAL(&mutex);
    state.link.abort(); state.rxEnabled = false;
    bool disconnect = state.link.connected && !state.disconnectIssued;
    if (disconnect) state.disconnectIssued = true;
    portEXIT_CRITICAL(&mutex);
    // If OPEN is pending, leave its interface and objects alive. A late
    // connection is disconnected by onEvent; a failed OPEN becomes idle.
    return disconnect ? esp_ble_gap_disconnect(s.peer) : ESP_OK;
  }
};
