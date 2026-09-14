/*
 * WireGuard implementation for ESP32 Arduino by Kenta Ida (fuga@fugafuga.org)
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once
#include <IPAddress.h>

class WireGuard
{
private:
    bool _is_initialized = false;
public:
    // keepaliveSeconds (local change): persistent keepalive interval in
    // seconds; 0 keeps the built-in KEEPALIVE_TIMEOUT (10 s).
    bool begin(const IPAddress& localIP, const char* privateKey, const char* remotePeerAddress, const char* remotePeerPublicKey, uint16_t remotePeerPort, uint16_t keepaliveSeconds = 0);
    void end();
    bool is_initialized() const { return this->_is_initialized; }
    // Local change: true once the peer handshake is done (the tunnel
    // carries traffic). Only meaningful while is_initialized().
    bool is_up() const;
};
