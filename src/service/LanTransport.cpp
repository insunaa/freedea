// Freedea — LAN session to one Midea AC (Phase 4.2). See LanTransport.h.
// Protocol flow per msmart lan.py (_LanProtocolV3 / LAN at d7db53b):
// V3 = TCP connect, handshake(token) -> derive local key from key, then
// frames ride as V2 packets sealed in AES-256-CBC V3 packets. V2 = V2
// packets directly on the socket.

#include "LanTransport.h"

#include <Arduino.h>
#include <esp_system.h>
#include <lwip/sockets.h>

#include <cstring>
#include <ctime>

#include <Frame.h>
#include <Packet.h>
#include <V3Packet.h>

// File-scope scratch used only from the acsvc task (single-threaded by
// construction): TCP stream assembly, V2 packet staging, decrypted V3
// payload, and V3 CBC pad bytes. Sized from the codec maxima below.
static uint8_t sWireBuf[512];
static uint8_t sTxPacket[384];
static uint8_t sInnerBuf[384];
static uint8_t sPad[midea::kAesBlockSize];

// Largest frame (256) -> V2 packet = 40 + PKCS#7(256) + 16 = 328.
static_assert(midea::v2PacketSize(midea::kFrameHeaderLen + midea::kFrameMaxDataLen + 1) <= sizeof(sTxPacket),
              "V2 staging buffer too small");
// That packet sealed in V3 = 6 + 2 + 328 + pad + 32 = 374.
static_assert(midea::v3EncryptedPacketSize(midea::v2PacketSize(midea::kFrameHeaderLen + midea::kFrameMaxDataLen + 1)) <=
                  sizeof(sWireBuf),
              "wire buffer too small");
// Decrypted V3 payload is the inner V2 packet; a handshake response is 64.
static_assert(midea::v2PacketSize(midea::kFrameHeaderLen + midea::kFrameMaxDataLen + 1) <= sizeof(sInnerBuf),
              "inner buffer too small");

// TCP keepalive on the AC socket: lwIP probes ~every 10 s of silence so the
// connection (and hopefully the AC's ~30 s session timer, 4.2) survives
// while the UI sits on screens that issue no app-layer polls. Whether the
// Porti counts bare keepalive ACKs toward its session timer is unverified —
// if it does not, the socket simply dies and AcService reopens on the next
// AC screen, so the probes are a pure optimization. ESP-IDF lwIP ships
// LWIP_TCP_KEEPALIVE=1, so the per-socket tunables below take effect.
static void enableTcpKeepalive(WiFiClient& client) {
  int on = 1;
  int rc = client.setSocketOption(SO_KEEPALIVE, reinterpret_cast<char*>(&on), sizeof(on));
  int idleSec = 10;
  int intervalSec = 5;
  int probeCount = 3;
  rc |= client.setSocketOption(IPPROTO_TCP, TCP_KEEPIDLE, &idleSec, sizeof(idleSec));
  rc |= client.setSocketOption(IPPROTO_TCP, TCP_KEEPINTVL, &intervalSec, sizeof(intervalSec));
  rc |= client.setSocketOption(IPPROTO_TCP, TCP_KEEPCNT, &probeCount, sizeof(probeCount));
  if (rc != 0) {
    Serial.printf("[AC ] keepalive setsockopt rc=%d (continuing without)\n", rc);
  }
}

// Fills the 8-byte V2 packet timestamp from the system clock. The clock is
// not SNTP-synced yet (a later phase); even unsynced the bytes are
// structurally valid, matching msmart, which never relies on the receiver
// validating them.
static void fillTimestamp(uint8_t out[midea::kV2PacketTimestampLen]) {
  const time_t now = time(nullptr);
  struct tm utc;
  gmtime_r(&now, &utc);
  midea::buildV2Timestamp(out, static_cast<uint16_t>(utc.tm_year + 1900), static_cast<uint8_t>(utc.tm_mon + 1),
                          static_cast<uint8_t>(utc.tm_mday), static_cast<uint8_t>(utc.tm_hour),
                          static_cast<uint8_t>(utc.tm_min), static_cast<uint8_t>(utc.tm_sec), 0);
}

bool LanTransport::open(const devices::Device& device) {
  close();
  if ((device.ip[0] | device.ip[1] | device.ip[2] | device.ip[3]) == 0) {
    Serial.println("[AC ] target has no IP yet, cannot connect");
    return false;
  }
  deviceId_ = device.id;
  v3Mode_ = device.version == 3 || device.hasCredentials();

  const IPAddress ip(device.ip[0], device.ip[1], device.ip[2], device.ip[3]);
  const unsigned ip0 = device.ip[0], ip1 = device.ip[1], ip2 = device.ip[2], ip3 = device.ip[3];
  if (client_.connect(ip, device.port, static_cast<int32_t>(kConnectTimeoutMs)) != 1) {
    Serial.printf("[AC ] connect %u.%u.%u.%u:%u failed\n", ip0, ip1, ip2, ip3, static_cast<unsigned>(device.port));
    return false;
  }
  open_ = true;
  enableTcpKeepalive(client_);
  Serial.printf("[AC ] tcp connected %u.%u.%u.%u:%u (%s)\n", ip0, ip1, ip2, ip3, static_cast<unsigned>(device.port),
                v3Mode_ ? "V3" : "V2");

  if (!v3Mode_) {
    return true;
  }
  if (!device.hasCredentials()) {
    Serial.println("[AC ] V3 device has no token/key, cannot authenticate");
    close();
    return false;
  }
  if (!handshake(device)) {
    close();
    return false;
  }
  Serial.println("[AC ] auth ok");
  return true;
}

void LanTransport::close() {
  client_.stop();
  open_ = false;
  authed_ = false;
  wireLen_ = 0;
}

bool LanTransport::authExpired() const {
  return authed_ && static_cast<uint32_t>(millis() - authedAtMs_) >= kAuthLifetimeMs;
}

bool LanTransport::handshake(const devices::Device& device) {
  size_t packetLen = 0;
  if (!midea::encodeV3HandshakeRequest(packetId_, device.token, device.tokenLen, sTxPacket, sizeof(sTxPacket),
                                       &packetLen)) {
    Serial.println("[AC ] handshake encode failed");
    return false;
  }
  if (client_.write(sTxPacket, packetLen) != packetLen) {
    Serial.println("[AC ] handshake write failed");
    return false;
  }
  packetId_ = static_cast<uint16_t>((packetId_ + 1) & kPacketIdMask);

  const Result r = accumulate(midea::kV3Start0, midea::kV3Start1, 8, 8, true, millis() + kAuthTimeoutMs);
  if (r != Result::kOk) {
    Serial.printf("[AC ] handshake read failed (result=%u)\n", static_cast<unsigned>(r));
    return false;
  }
  const midea::V3PacketType type = midea::v3PacketType(sWireBuf);
  if (type != midea::V3PacketType::HandshakeResponse) {
    Serial.printf("[AC ] expected handshake response, got type 0x%x\n", static_cast<unsigned>(type));
    return false;
  }
  size_t payloadLen = 0;
  if (!midea::decodeV3Packet(nullptr, sWireBuf, wireLen_, sInnerBuf, sizeof(sInnerBuf), &payloadLen)) {
    Serial.println("[AC ] handshake packet decode failed");
    return false;
  }
  if (payloadLen != 64) {
    Serial.printf("[AC ] handshake payload %u bytes, expected 64\n", static_cast<unsigned>(payloadLen));
    return false;
  }
  if (!midea::deriveV3LocalKey(device.key, sInnerBuf, localKey_)) {
    Serial.println("[AC ] local key derivation failed (wrong key?)");
    return false;
  }
  // The handshake response is consumed here; leaving it buffered would make
  // the next readFrame() see a non-encrypted-response packet.
  wireLen_ = 0;
  authed_ = true;
  authedAtMs_ = millis();
  return true;
}

LanTransport::Result LanTransport::sendFrame(const uint8_t* frame, size_t frameLen) {
  if (!alive()) {
    return Result::kLinkDown;
  }
  if (v3Mode_ && !authed_) {
    return Result::kAuthError;
  }

  uint8_t timestamp[midea::kV2PacketTimestampLen];
  fillTimestamp(timestamp);
  size_t packetLen = 0;
  if (!midea::encodeV2Packet(frame, frameLen, deviceId_, timestamp, sTxPacket, sizeof(sTxPacket), &packetLen)) {
    Serial.println("[AC ] V2 packet encode failed");
    return Result::kProtocolError;
  }

  const uint8_t* out = sTxPacket;
  size_t outLen = packetLen;
  if (v3Mode_) {
    const size_t pad = midea::v3PadSize(packetLen);
    if (pad > 0) {
      esp_fill_random(sPad, pad);
    }
    if (!midea::encodeV3EncryptedRequest(localKey_, packetId_, sTxPacket, packetLen, sPad, sWireBuf, sizeof(sWireBuf),
                                         &outLen)) {
      Serial.println("[AC ] V3 packet encode failed");
      return Result::kProtocolError;
    }
    packetId_ = static_cast<uint16_t>((packetId_ + 1) & kPacketIdMask);
    out = sWireBuf; // consumed by write() below before wireLen_ reuses it
  }
  if (client_.write(out, outLen) != outLen) {
    Serial.println("[AC ] write failed (link down)");
    return Result::kLinkDown;
  }
  // The read stream is deliberately not reset: pushes queued ahead of (or
  // behind) the response stay buffered, and a partial packet in the buffer
  // is the prefix of one the device is still transmitting.
  return Result::kOk;
}

LanTransport::Result LanTransport::readFrame(uint8_t* frameOut, size_t frameCap, size_t* frameLen,
                                             uint32_t deadlineMs) {
  if (!alive()) {
    return Result::kLinkDown;
  }
  if (v3Mode_) {
    return readFrameV3(frameOut, frameCap, frameLen, deadlineMs);
  }
  const Result r = accumulate(0x5A, 0x5A, 6, midea::kV2PacketMinLen, false, deadlineMs);
  if (r != Result::kOk) {
    return r;
  }
  const size_t total = frontPacketTotal();
  if (!midea::decodeV2Packet(sWireBuf, wireLen_, frameOut, frameCap, frameLen)) {
    Serial.println("[AC ] V2 packet decode failed");
    return Result::kProtocolError;
  }
  consumeFront(total);
  return Result::kOk;
}

bool LanTransport::hasReadableData() {
  if (!open_) {
    return false;
  }
  return frontPacketTotal() != 0 || client_.available() > 0;
}

size_t LanTransport::frontPacketTotal() const {
  if (v3Mode_) {
    if (wireLen_ < 8 || sWireBuf[0] != midea::kV3Start0 || sWireBuf[1] != midea::kV3Start1) {
      return 0;
    }
    const size_t total = ((static_cast<size_t>(sWireBuf[2]) << 8) | sWireBuf[3]) + 8;
    return total <= wireLen_ ? total : 0;
  }
  if (wireLen_ < 6 || sWireBuf[0] != 0x5A || sWireBuf[1] != 0x5A) {
    return 0;
  }
  const size_t total = static_cast<size_t>(sWireBuf[4]) | (static_cast<size_t>(sWireBuf[5]) << 8);
  return total <= wireLen_ ? total : 0;
}

void LanTransport::consumeFront(size_t total) {
  if (total >= wireLen_) {
    wireLen_ = 0;
    return;
  }
  wireLen_ -= total;
  memmove(sWireBuf, sWireBuf + total, wireLen_);
}

LanTransport::Result LanTransport::readFrameV3(uint8_t* frameOut, size_t frameCap, size_t* frameLen,
                                               uint32_t deadlineMs) {
  const Result r = accumulate(midea::kV3Start0, midea::kV3Start1, 8, 8, true, deadlineMs);
  if (r != Result::kOk) {
    return r;
  }
  const midea::V3PacketType type = midea::v3PacketType(sWireBuf);
  if (type != midea::V3PacketType::EncryptedResponse) {
    Serial.printf("[AC ] unexpected V3 packet type 0x%x\n", static_cast<unsigned>(type));
    return Result::kProtocolError;
  }
  const size_t total = frontPacketTotal();
  size_t innerLen = 0;
  if (!midea::decodeV3Packet(localKey_, sWireBuf, wireLen_, sInnerBuf, sizeof(sInnerBuf), &innerLen)) {
    Serial.println("[AC ] V3 packet failed to decrypt/verify");
    return Result::kProtocolError;
  }
  if (!midea::decodeV2Packet(sInnerBuf, innerLen, frameOut, frameCap, frameLen)) {
    Serial.println("[AC ] inner V2 packet failed to decode");
    return Result::kProtocolError;
  }
  consumeFront(total);
  return Result::kOk;
}

LanTransport::Result LanTransport::accumulate(uint8_t magic0, uint8_t magic1, size_t headerLen, size_t minTotal,
                                              bool bigEndianSize, uint32_t deadlineMs) {
  for (;;) {
    // Resync: drop bytes until the buffer starts with the packet magic.
    while (wireLen_ >= 2 && (sWireBuf[0] != magic0 || sWireBuf[1] != magic1)) {
      wireLen_ -= 1;
      memmove(sWireBuf, sWireBuf + 1, wireLen_);
    }
    if (wireLen_ >= headerLen) {
      const size_t total = bigEndianSize ? ((static_cast<size_t>(sWireBuf[2]) << 8) | sWireBuf[3]) + 8
                                         : (static_cast<size_t>(sWireBuf[4]) | (static_cast<size_t>(sWireBuf[5]) << 8));
      if (total < minTotal || total > sizeof(sWireBuf)) {
        Serial.printf("[AC ] bad packet size %u\n", static_cast<unsigned>(total));
        wireLen_ = 0;
        return Result::kProtocolError;
      }
      if (wireLen_ >= total) {
        // Complete packet at the front. Anything queued behind it (device
        // pushes, pipelined responses) stays buffered for the next read.
#if FREEDDEA_FAULT_INJECT
        // 5.2 test hook (env x4_faultinject only): corrupt every 4th fully
        // received packet after reassembly and before the caller validates
        // it — payload corruption (CRC/decrypt failure) plus, every other
        // time, size-field corruption (short/oversized declared length ->
        // stream desync + resync). Drives the real protocol-error paths end
        // to end: kProtocolError must only log + toast + reconnect, never
        // crash.
        static uint32_t sInjectedPackets = 0;
        if (++sInjectedPackets % 4 == 0) {
          const size_t idx = (sInjectedPackets % 8 == 0) ? (bigEndianSize ? 2 : 4) : headerLen + 2;
          if (idx < total && idx < wireLen_) {
            sWireBuf[idx] ^= 0xA5;
            Serial.printf("[AC ] FAULT: corrupted byte %u of packet (size %u)\n", static_cast<unsigned>(idx),
                          static_cast<unsigned>(total));
          }
        }
#endif
        return Result::kOk;
      }
    }
    if (client_.connected() == 0) {
      wireLen_ = 0;
      return Result::kLinkDown;
    }
    if (static_cast<int32_t>(millis() - deadlineMs) >= 0) {
      return Result::kTimeout; // partial data stays buffered for the next call
    }
    if (wireLen_ == sizeof(sWireBuf)) {
      // No room for the rest of the front packet: a push burst queued more
      // than one buffer holds. Discard and resync on the next magic.
      Serial.println("[AC ] wire buffer full; resyncing stream");
      wireLen_ = 0;
      continue;
    }
    const int available = client_.available();
    if (available > 0) {
      const size_t space = sizeof(sWireBuf) - wireLen_;
      const int n = client_.read(sWireBuf + wireLen_,
                                 space < static_cast<size_t>(available) ? space : static_cast<size_t>(available));
      if (n < 0) {
        wireLen_ = 0;
        return Result::kLinkDown;
      }
      wireLen_ += static_cast<size_t>(n);
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}
