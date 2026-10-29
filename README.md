# ESP_BMS

Balisto live BMS dashboard for ESP32 — R7.

Firmware: **BMS-2026-10-16-R7**. ESP32 Arduino core used for validation: **3.3.9**, classic Bluedroid stack.

## Upload

Clone using the Arduino sketch folder name, which must match the `.ino` filename:

```sh
git clone https://github.com/Naresh196c/ESP_BMS.git Balisto_Live_BMS_Dashboard_Names
```

1. Open `Balisto_Live_BMS_Dashboard_Names.ino` in Arduino IDE from this folder. Keep `bms_gatt.h`, `bms_protocol.h`, and your local `secrets.h` beside it.
2. On a fresh clone, copy `secrets.example.h` to `secrets.h`. Set `AP_NAME`, `AP_PASS`, `ROUTER_SSID`, and `ROUTER_PASS` in `secrets.h`. Empty router values enable the Balisto-BMS hotspot instead. Existing local credentials are preserved. `secrets.h`, backup files and build logs are excluded from Git; commit only the example settings.
3. Select the board matching your physical module and your USB port. A normal ESP32-WROOM development board normally uses **ESP32 Dev Module**; select **ESP32-WROOM-DA Module** only for the DA hardware. The currently saved validation settings use WROOM-DA, so verify the hardware selection yourself. Use a partition large enough for Wi-Fi and BLE, such as **Huge APP**.
4. Upload. At 115200 baud, confirm `BMS-2026-10-16-R7` and `Persistent native GATT client registered; no per-battery stack restart.`
5. Connect the browser to the same router and open the Website address printed in Serial Monitor, adding `/?r=7`. DHCP may change the address; do not assume the previous 192.168.1.87 address is permanent.
6. The dashboard loads available readings immediately, then refreshes every **five minutes**. **Refresh now** retrieves existing readings; it does not start a new BLE sweep. A large fleet pass can exceed five minutes and is allowed to finish.

Hotspot mode: connect to `Balisto-BMS` using the configured `AP_PASS`, then open the printed hotspot address (normally `http://192.168.4.1/?r=7`).

## Why R5 crashed

The supplied panic used ELF SHA `bea1fcbab`, which matched the saved ELF. Its addresses decoded through `gap_find_clcb_by_bd_addr`, GATT/L2CAP disconnect cleanup, `l2c_free`, `BTU_ShutDown`, and `bte_main_shutdown`. R5 shut down Bluetooth after an application connection timeout while controller cleanup could still be outstanding. This was a software shutdown failure, not evidence of insufficient Bluetooth range.

## What R7 changes

The R6 hardware log contains successful SOC reads and responsive HTTP pages, followed by a discovery timeout and an unconfirmed disconnect. R7 corrects the short cleanup deadline and permanent polling stop in that path. Signal readings around -84 to -91 dBm also indicate a weak received signal; compare the same batteries with the ESP32 nearby to separate radio conditions from firmware behavior.

- **Removes all per-battery BLE stack shutdown/reinitialization and BLEClient deletion.** One native GATT application remains registered for the lifetime of the boot. It uses the existing Arduino BLE stack and scanner; no additional library installation is required.
- **Does not unregister the client when an application deadline expires.** The controller still owns that pending OPEN. R7 waits for its completion; a late successful connection is disconnected before another battery can use the slot.
- **Waits for actual disconnect confirmation.** No next peer is started while the previous OPEN or physical connection remains active.
- **Bounds discovery and subscription waits.** OPEN wait: 15 seconds; automatic service discovery: 20 seconds; service search, notification registration, descriptor write and command acknowledgement: 5 seconds each; FF04 status wait: 6 seconds. These are separate phase deadlines, not a single total transaction limit. Healthy connections continue as soon as their events arrive; these values do not impose a minimum wait.
- **Allows 35 seconds for disconnect confirmation**, including established connections. R6 allowed only five seconds for those connections. If cleanup still remains unresolved, polling waits with a visible diagnostic. When the pending OPEN and physical connection are confirmed released, polling resumes automatically at the remaining batteries in the same pass. A virtual CLOSE event alone does not release the physical slot. Wi-Fi and saved readings remain available throughout. If no terminal event ever arrives, a manual reset is still needed; there is no forced stack shutdown or automatic reboot.
- **Logs RSSI, native OPEN/GATT statuses and disconnect reasons** so radio failures can be distinguished from protocol and firmware failures.
- **Parses notifications as a stream.** Headers may span notifications, and frames may be concatenated or preceded by garbage. Fixed buffers avoid allocations. A validated reading still uses the original FF04 field offsets, XOR and end marker. The original eight-byte FF04 request and the 250 ms notification-settling interval are preserved.
- **Reports rejected response details:** received notification/byte counts, buffered and expected length, last validation error, and up to 64 received bytes in hex. Errors distinguish missing data, incomplete frames, invalid lengths, wrong response commands, checksum mismatch, trailer mismatch and SOC outside 0–100.
- **Synchronizes callback state** with a short critical section. All SDK calls, printing, waiting and web work occur outside that critical section. Callback storage and its owner remain alive for the entire boot.

The existing filtered scanner, full advertised names, eight-record HTTP pages, explicit response lengths, five-minute cycle target and 30-minute retention remain. Advertised address types are preserved. All detected compatible candidates are attempted once per pass, strongest RSSI first, subject to available memory.

## Diagnostics

- `/health` displays firmware, selected board, heap, current stage, `ble_faulted`, `ble_cleanup_pending`, `ble_cleanup_failures`, pending OPEN/physical connection state and virtual CLOSE diagnostics.
- `native OPEN status=0x....` is a completed controller result; `OPEN deadline reached` means its completion has not arrived yet.
- `Connection released` confirms that the slot is idle before the next peer.
- `Status response: ...` describes a missing or rejected FF04 frame. Use the printed MAC and RSSI to compare the same battery when moved closer.
- `BLE quarantined` means controller cleanup is still pending; no other connection is started. `Late controller cleanup confirmed` means the remaining queue resumes without a reset. Resume returns HTTP 202 while cleanup is pending; HTTP 409 is reserved for a failed client registration.
- Serial `P` or **Pause Bluetooth** pauses after the current transaction; `R` or **Resume Bluetooth** resumes only if the controller is in a confirmed state.

## Validation

- The final R7 sketch compiles with the installed ESP32 compiler and links with matching cached Arduino 3.3.9 core/library objects using `tests/compile-sketch.ps1 -Link`. This does not flash the board or prove radio behavior.
- `tests/compile-sketch.ps1` is a helper for the original Windows development machine: it requires that machine's cached Arduino compilation database/core/library objects and, for `-Link`, its ignored `build-r4.log`. On a fresh clone, configure `secrets.h` and use Arduino IDE to compile instead.
- `tests/gatt.test.cpp` runs the actual GATT adapter against a fake SDK. It checks persistent registration, refusal to overlap peers, FFE0/FFE1/CCCD setup, the exact verified FF04 command, notification identity filtering, OPEN failures, late connection cleanup, virtual CLOSE versus physical DISCONNECT, and absence of SDK calls inside critical sections.
- `tests/protocol.test.cpp` tests every header split, randomly fragmented notifications, stream resynchronization, malformed frames, checksum/SOC/command/trailer failures, partial data, a 100 KB noise input, and late-OPEN/disconnect lifecycle ordering.
- `node tests/dashboard.test.cjs` checks a 93-device paginated display, partial transfers, invalid JSON, empty lists, timeouts, firmware mismatch, sequential requests, and the five-minute browser interval.
- `python tests/api_format_test.py` checks actual C formatting for bounded, valid JSON across page sizes and worst-case field values on Windows.
- Hardware operation remains to be checked by uploading R7 and observing at least one full fleet pass, including failed connections. No automatic upload was performed.

## Sources

- [Espressif ESP-IDF 5.5.4 GATT client example](https://github.com/espressif/esp-idf/blob/v5.5.4/examples/bluetooth/bluedroid/ble/gatt_client/main/gattc_demo.c)
- [Espressif GATT client API and connection/disconnect events](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32/api-reference/bluetooth/esp_gattc.html)
- [Espressif RF coexistence: one shared radio; station and SoftAP support differ](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32/api-guides/coexist.html)
- [Espressif fatal-error interpretation](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32/api-guides/fatal-errors.html#loadprohibited-storeprohibited)
