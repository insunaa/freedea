#pragma once

// Freedea — on-demand LAN discovery scan (Phase 4.4).
//
// Broadcasts the Midea discovery datagram (midea::kDiscoveryMessage, ported
// from msmart discover.py) to both discovery ports and collects replies for
// a fixed window. Driven entirely from the main loop: start() once, then
// poll() every iteration — poll() is non-blocking, so the UI keeps servicing
// buttons while the scan runs. The UDP socket exists only during a scan
// (begin()/stop()); all other memory is a fixed .bss table, no heap.
//
// Single-task by construction (main loop only), so no locking: poll(), the
// accessors and consumeScanDoneEdge() must all be called from the loop task.

#include <WiFiUdp.h>

#include <cstddef>
#include <cstdint>

#include <Discovery.h>

class DeviceDiscovery {
public:
  // Fixed result table cap (deduplicated by device id); overflow replies are
  // logged and dropped.
  static constexpr size_t kMaxResults = 8;
  // Datagram read cap; observed discovery replies are < 250 bytes, anything
  // larger is treated as garbage (the read is truncated, not parsed).
  static constexpr size_t kMaxDatagramLen = 512;

  // Begin a scan: bind UDP, send the first probe round, open the listen
  // window. No-op while already scanning; on UDP failure nothing starts and
  // false is returned (status is also logged).
  bool start();

  // Drain pending datagrams, fire scheduled probe rounds, close the window.
  // Call every loop iteration; free when idle.
  void poll();

  // Abort a running scan and free the socket. Safe in any state.
  void cancel();

  bool scanning() const { return scanning_; }

  // True once after a scan completes normally (not cancel()); cleared by the
  // read. The main loop consumes it to repaint the Devices screen, so no
  // repaint happens per datagram (e-ink discipline).
  bool consumeScanDoneEdge();

  // Results of the most recent completed scan. Valid until the next start().
  size_t count() const { return count_; }
  const midea::DiscoveryDeviceInfo& at(size_t index) const { return results_[index]; }

private:
  void sendProbeRound();
  void handleDatagram(int len);
  void finish(bool doneEdge);

  WiFiUDP udp_;
  bool scanning_ = false;
  bool doneEdge_ = false;
  uint32_t startedAtMs_ = 0;
  uint8_t roundsSent_ = 0;
  size_t count_ = 0;
  midea::DiscoveryDeviceInfo results_[kMaxResults];

  // Datagram scratch: one buffer shared by all reads, never on the stack.
  uint8_t rxBuf_[kMaxDatagramLen];
};
