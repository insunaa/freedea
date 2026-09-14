#pragma once

// Freedea — LAN session to one Midea AC (Phase 4.2). V3-first: TCP connect,
// token handshake (derive the AES-256 session key, 12 h lifetime per
// msmart lan.py), then exchange frames as V3-encrypted packets wrapping V2
// packets. The unauthenticated V2 stream (5A5A packets directly on the
// socket) is kept as the stretch path for stored devices with no token.
//
// Single-task use (the acsvc task); no locking. Stream handling mirrors
// msmart _LanProtocolV3.data_received: resync past bytes before the magic,
// take the BE16 size field + 8 as the wire length, and resume partial
// packets across calls.
//
// The stream is request-agnostic: devices push unsolicited frames (state
// after a physical-remote change, queries), so sendFrame() never clears the
// read side and readFrame() consumes exactly one complete frame — response
// or push — leaving anything queued behind it for the next call.

#include <WiFiClient.h>

#include <Devices.h>

#include <cstddef>
#include <cstdint>

class LanTransport {
public:
  enum class Result : uint8_t {
    kOk = 0,
    kTimeout,       // deadline passed with no complete packet
    kLinkDown,      // TCP died (peer closed, or a write failed)
    kProtocolError, // framing, checksum or crypto failure; restart the session
    kAuthError,     // V3 handshake rejected or key derivation failed
  };

  ~LanTransport() { close(); }

  // TCP connect to the device; when it is V3 (version 3 or credentials
  // present) run the handshake so exchange() is usable. Logs "auth ok" on a
  // successful handshake. False on any failure, with the socket closed.
  bool open(const devices::Device& device);
  void close();

  // Socket open and the peer still connected (NetworkClient::connected()
  // is non-const, so this is too).
  bool alive() { return open_ && client_.connected() != 0; }

  // True when the V3 session key passed its 12 h lifetime: the caller should
  // close() and reopen (msmart expires authentication the same way).
  bool authExpired() const;

  // Send one frame (V2 packet, sealed in V3 when authenticated). Does not
  // read: call readFrame() for the response. The read stream is untouched,
  // so device pushes queued ahead of the response stay readable.
  Result sendFrame(const uint8_t* frame, size_t frameLen);

  // Read the next complete frame (response or unsolicited push) off the
  // stream, waiting until the absolute deadlineMs. On kOk the frame is fully
  // consumed; any further buffered bytes stay for the next call. kTimeout
  // leaves partial data buffered; kLinkDown/kProtocolError mean the session
  // is unusable and the caller must close().
  Result readFrame(uint8_t* frameOut, size_t frameCap, size_t* frameLen, uint32_t deadlineMs);

  // Cheap hint that readFrame() could yield a frame now (bytes in flight or
  // a complete frame already buffered); lets the task poll for pushes
  // between its ticks without blocking.
  bool hasReadableData();

private:
  // msmart LAN uses a 5 s connect timeout; reads get 3 s per attempt.
  static constexpr uint32_t kConnectTimeoutMs = 5000;
  static constexpr uint32_t kAuthTimeoutMs = 3000;
  // AUTHENTICATION_EXPIRATION in lan.py, in milliseconds (fits uint32).
  static constexpr uint32_t kAuthLifetimeMs = 12u * 3600u * 1000u;
  // V3 packet id counter: increments per write, masked to 12 bits (lan.py).
  static constexpr uint16_t kPacketIdMask = 0x0FFF;

  bool handshake(const devices::Device& device);

  // Accumulate exactly one wire packet at the front of the wire buffer.
  // Drops leading bytes that are not the magic; `headerLen` is the minimum
  // bytes needed before the declared total is readable, `minTotal` rejects
  // nonsense sizes, and `bigEndianSize` selects the BE16+8 (V3) vs LE16 (V2)
  // size field. Partial data stays buffered for the next call.
  Result accumulate(uint8_t magic0, uint8_t magic1, size_t headerLen, size_t minTotal, bool bigEndianSize,
                    uint32_t deadlineMs);

  // V3: expect one encrypted response (type 3), decrypt it, then decode the
  // inner V2 packet into frameOut and consume it from the wire buffer.
  Result readFrameV3(uint8_t* frameOut, size_t frameCap, size_t* frameLen, uint32_t deadlineMs);

  // Wire length of the complete magic packet at the buffer front, or 0 when
  // it is not (yet) a determinable full packet. Mirrors accumulate()'s math.
  size_t frontPacketTotal() const;
  // Remove the front packet's bytes, keeping anything queued behind it.
  void consumeFront(size_t total);

  WiFiClient client_;
  uint8_t localKey_[32] = {};
  uint64_t deviceId_ = 0;
  uint32_t authedAtMs_ = 0;
  uint16_t packetId_ = 0;
  size_t wireLen_ = 0;
  bool v3Mode_ = false;
  bool authed_ = false;
  bool open_ = false;
};
