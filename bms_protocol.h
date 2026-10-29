#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Platform-independent stream parser. Callers serialize feed/reset/snapshot.
// Handles split headers, concatenated frames and garbage without allocating.



struct FF04Receiver {
  uint8_t buffer[128] = {};
  uint8_t lastFrame[128] = {};
  size_t used = 0, expected = 0, lastLength = 0;
  uint32_t notifications = 0, bytes = 0, discarded = 0, rejected = 0;
  const char* reason = "No notifications";
  const char* lastReject = "None";
  bool ready = false;
  int soc = -1;
  float voltage = 0;
  uint16_t cycles = 0;

  void reset() { *this = FF04Receiver(); }
  static const char* validate(const uint8_t* p, size_t n) {
    if (n < 51) return "Frame too short";
    if (p[0] != 0xEA || p[1] != 0xD1 || p[2] != 0x01) return "Header mismatch";
    if (n != size_t(4 + p[3])) return "Length mismatch";
    if (p[4] != 0xFF || p[5] != 0x04) return "Not an FF04 response";
    if (p[n - 1] != 0xF5) return "Trailer mismatch";
    uint8_t check = 0;
    for (size_t i = 3; i < n - 2; ++i) check ^= p[i];
    if (check != p[n - 2]) return "Checksum mismatch";
    if (p[7] > 100) return "SOC outside 0..100";
    return nullptr;
  }
  void shift() {
    if (used) { --used; memmove(buffer, buffer + 1, used); }
    expected = 0;
  }
  void feed(const uint8_t* p, size_t n) {
    ++notifications; bytes += n;
    for (size_t i = 0; i < n && !ready; ++i) {
      buffer[used++] = p[i];
      while (used && !ready) {
        if (buffer[0] != 0xEA || (used >= 2 && buffer[1] != 0xD1) ||
            (used >= 3 && buffer[2] != 0x01)) {
          ++discarded; reason = "No FF04 header"; shift(); continue;
        }
        if (used < 4) { reason = "Partial header"; break; }
        expected = size_t(4) + buffer[3];
        if (expected < 51 || expected > sizeof(buffer)) {
          ++rejected; reason = "Invalid frame length"; lastReject = reason; shift(); continue;
        }
        if (used < expected) { reason = "Partial frame"; break; }
        lastLength = expected;
        memcpy(lastFrame, buffer, expected);
        const char* failure = validate(buffer, expected);
        if (!failure) {
          soc = buffer[7];
          voltage = ((uint16_t(buffer[47]) << 8) | buffer[48]) / 100.0f;
          cycles = (uint16_t(buffer[9]) << 8) | buffer[10];
          ready = true; reason = "OK";
        } else {
          ++rejected; reason = failure; lastReject = failure; shift();
        }
      }
    }
  }
};

// A timed-out OPEN is still owned by the controller. Never reuse its slot
// until OPEN has completed AND any physical connection has disconnected.
struct BmsLinkLifecycle {
  bool opening = false, connected = false, abandoned = false;
  uint16_t connection = 0xFFFF;
  bool idle() const { return !opening && !connected; }
  bool begin() {
    if (!idle()) return false;
    opening = true; abandoned = false; connection = 0xFFFF; return true;
  }
  void onConnected(uint16_t id) { connected = true; connection = id; }
  void onOpen(bool success, uint16_t id) {
    opening = false;
    if (success) onConnected(id);
  }
  void abort() { abandoned = true; }
  void onDisconnected() { connected = false; connection = 0xFFFF; }
};
