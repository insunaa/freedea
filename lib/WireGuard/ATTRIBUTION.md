# WireGuard for ESP32 (Arduino) — vendored

Vendored from https://github.com/ciniml/WireGuard-ESP32-Arduino, tag `0.1.5`
(commit `a3307979bb5bfe4e767a9331f06cf1bc05136b1b`), BSD-3-Clause (see
`LICENSE`). That library is itself a port of `smartalock/wireguard-lwip`
(Daniel Hope, Floorsense Ltd — also in `LICENSE`) to the Arduino ESP32 core.

Pure C sources: lwIP `netif` WireGuard implementation with reference-C
crypto (curve25519 / chacha20poly1305 / blake2s / poly1305-donna) and RNG
via the Arduino core's mbedTLS entropy — no new crypto dependency.

## Local changes (Freedea)

1. `src/wireguardif.c` (`wireguardif_shutdown`): free the device context
   with `mem_free()` instead of libc `free()` — it is `mem_calloc()`'d in
   `wireguardif_init()`, so the upstream mismatch corrupted the heap on
   every tunnel shutdown.
2. `src/WireGuard.cpp` / `WireGuard-ESP32.h` (`begin`): added an optional
   `keepaliveSeconds` parameter (0 = upstream 10 s default) that sets
   `peer.keep_alive`. The persistent keepalive wakes the radio every
   interval; a battery device needs to raise it.
3. `src/WireGuard.cpp` / `WireGuard-ESP32.h`: added `is_up()` accessor
   around `wireguardif_peer_is_up()` so the app can show tunnel state
   without touching the file-scope netif statics.
4. `src/wireguardif.c`: the legacy `tcpip_adapter.h` compat header is not
   shipped by the ESP-IDF 5.x Arduino core; the underlying STA `struct
   netif` is now fetched via `esp_netif_get_handle_from_ifkey
   ("WIFI_STA_DEF")` + `esp_netif_get_netif_impl()`.
5. `src/wireguard.h`: added an explicit `handshake_destroy()` prototype —
   wireguardif.c calls it and GCC 13+ (pioarduino 55.03 core) rejects the
   implicit declaration older toolchains only warned about.
6. `src/WireGuard.cpp` (`begin()`/`end()` rewrite): the ESP-IDF 5.x lwIP
   enforces core-lock ownership (`LWIP_ASSERT_CORE_LOCKED`) — upstream's
   direct `netif_add()`/`netif_set_up()`/`netif_set_default()`/`netif_remove()`
   calls from the app task hit `assert ... Required to lock TCPIP core
   functionality!` and panic on first use (older toolchains let the raw
   calls through). Endpoint DNS stays on the caller's task (it blocks);
   the core-locked sequence (netif add + `wireguardif_init`'s raw
   `udp_new`/`udp_bind`, peer add, connect, default-netif swap, and the
   whole teardown) is dispatched to the tcpip thread via `tcpip_callback()`
   and awaited on a semaphore. `begin()` also checks `wireguardif_add_peer()`
   now (upstream ignored it) and closes the half-open netif on failure.
   A `begin()` while already initialized is rejected.
7. `src/wireguardif.c`: `FREEDDEA_DEBUG_WG`-gated `printf()` lines logging
   each handshake initiation's `udp_sendto_if()` result and every UDP packet
   arriving on the WG listen port. Debug flag is set only in the dev build
   env; the lines compile out entirely for releases.

## Usage constraints (measured/observed, Phase 7.3)

- `begin()` resolves the endpoint with `lwip_getaddrinfo()` and retries up
  to 5x with 2 s delays — call it from a task, never the main loop.
- `begin()` makes the WG netif the lwIP default interface, so **all**
  outbound traffic rides the tunnel while it is up (AllowedIPs 0.0.0.0/0).
  The WG server must route/NAT what the device needs.
- Single peer (`WIREGUARD_MAX_PEERS 1`), no preshared key, device state is
  allocated from the lwIP heap (`mem_calloc`).
