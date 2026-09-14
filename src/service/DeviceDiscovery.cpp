#include "DeviceDiscovery.h"

#include <Arduino.h>

#include <cinttypes>

// msmart discover.py cadence: 3 datagrams per port, then listen 5 s. Packets
// are spread over three rounds 1.5 s apart (same 3-per-port budget, one round
// per port each) so a single lost round does not hide a device; poll() also
// doubles as the round scheduler, keeping everything off blocking waits.
static constexpr uint32_t kProbeRoundIntervalMs = 1500;
static constexpr uint32_t kScanWindowMs = 5000;
static constexpr uint8_t kProbeRounds = 3;

bool DeviceDiscovery::start() {
  if (scanning_) return true;
  if (udp_.begin(0) == 0) {
    Serial.printf("[DISC] UDP bind failed, heap %u\n", static_cast<unsigned>(ESP.getFreeHeap()));
    return false;
  }
  Serial.printf("[DISC] scan start, heap %u\n", static_cast<unsigned>(ESP.getFreeHeap()));
  count_ = 0;
  roundsSent_ = 0;
  startedAtMs_ = millis();
  scanning_ = true;
  sendProbeRound();
  return true;
}

void DeviceDiscovery::sendProbeRound() {
  const uint16_t ports[] = {midea::kDiscoveryPortPrimary, midea::kDiscoveryPortSecondary};
  for (uint16_t port : ports) {
    if (udp_.beginPacket(IPAddress(255, 255, 255, 255), port) == 0 ||
        udp_.write(midea::kDiscoveryMessage, midea::kDiscoveryMessageLen) != midea::kDiscoveryMessageLen ||
        udp_.endPacket() == 0) {
      Serial.printf("[DISC] probe to port %u failed\n", static_cast<unsigned>(port));
    }
  }
  ++roundsSent_;
}

void DeviceDiscovery::handleDatagram(int len) {
  if (len <= 0) return;
  if (static_cast<size_t>(len) > kMaxDatagramLen) {
    // Oversized reply: drain it so the next parsePacket() sees the next
    // datagram, then ignore this one.
    udp_.clear();
    Serial.printf("[DISC] oversized reply (%d bytes) dropped\n", len);
    return;
  }
  if (udp_.read(rxBuf_, static_cast<size_t>(len)) != len) return;

  const midea::DiscoveryVersion version = midea::getDiscoveryVersion(rxBuf_, static_cast<size_t>(len));
  if (version == midea::DiscoveryVersion::kUnsupported) return; // unrelated LAN traffic

  const IPAddress src = udp_.remoteIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u", static_cast<unsigned>(src[0]), static_cast<unsigned>(src[1]),
           static_cast<unsigned>(src[2]), static_cast<unsigned>(src[3]));

  midea::DiscoveryDeviceInfo info;
  const midea::DiscoveryError err =
      midea::parseDiscoveryResponse(rxBuf_, static_cast<size_t>(len), version, ipStr, &info);
  if (err != midea::DiscoveryError::kNone) {
    Serial.printf("[DISC] bad reply from %s (%d bytes, err %u)\n", ipStr, len, static_cast<unsigned>(err));
    return;
  }

  // Deduplicate by device id: a repeat refreshes the entry (and its IP).
  for (size_t i = 0; i < count_; ++i) {
    if (results_[i].deviceId == info.deviceId) {
      results_[i] = info;
      return;
    }
  }
  if (count_ >= kMaxResults) {
    Serial.printf("[DISC] table full, dropping id %" PRIu64 " from %s\n", info.deviceId, ipStr);
    return;
  }
  results_[count_++] = info;
  Serial.printf("[DISC] found id=%" PRIu64 " v%u ip=%s port=%u name=%s\n", info.deviceId,
                static_cast<unsigned>(info.version), info.ip, static_cast<unsigned>(info.port), info.name);
}

void DeviceDiscovery::poll() {
  if (!scanning_) return;

  int len;
  while ((len = udp_.parsePacket()) > 0)
    handleDatagram(len);

  const uint32_t elapsed = millis() - startedAtMs_;
  if (roundsSent_ < kProbeRounds && elapsed >= roundsSent_ * kProbeRoundIntervalMs) sendProbeRound();
  if (elapsed >= kScanWindowMs) finish(true);
}

void DeviceDiscovery::cancel() {
  if (scanning_) finish(false);
  doneEdge_ = false;
}

void DeviceDiscovery::finish(bool doneEdge) {
  udp_.stop();
  scanning_ = false;
  doneEdge_ = doneEdge;
  Serial.printf("[DISC] scan done: %u device(s), heap %u\n", static_cast<unsigned>(count_),
                static_cast<unsigned>(ESP.getFreeHeap()));
}

bool DeviceDiscovery::consumeScanDoneEdge() {
  const bool edge = doneEdge_;
  doneEdge_ = false;
  return edge;
}
