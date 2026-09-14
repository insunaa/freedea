/*
 * WireGuard implementation for ESP32 Arduino by Kenta Ida (fuga@fugafuga.org)
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Freedea local changes: see ATTRIBUTION.md (keepalive parameter, is_up(),
 * and — the big one — all core-locked lwIP calls moved onto the tcpip
 * thread via tcpip_callback(), because the ESP-IDF 5.x lwIP enforces
 * LWIP_ASSERT_CORE_LOCKED() and the upstream 0.1.5 calls netif_add() etc.
 * straight from the app task, which asserts).
 */
#include "WireGuard-ESP32.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include <new>

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/ip.h"
#include "lwip/netdb.h"
#include "lwip/tcpip.h"

#include "esp32-hal-log.h"

extern "C" {
#include "wireguardif.h"
#include "wireguard-platform.h"
}

// Wireguard instance
static struct netif wg_netif_struct = {0};
static struct netif *wg_netif = NULL;
static struct netif *previous_default_netif = NULL;
static uint8_t wireguard_peer_index = WIREGUARDIF_INVALID_INDEX;

#define TAG "[WireGuard] "

namespace {

// Parameter hand-off into the tcpip-thread begin half. The caller blocks on
// the completion semaphore for the whole dispatch, so a caller-lifetime
// object is safe.
struct BeginCoreArgs {
	ip_addr_t ipaddr;
	ip_addr_t netmask;
	ip_addr_t gateway;
	ip_addr_t endpoint_ip;
	const char *private_key;
	const char *peer_public_key;
	uint16_t listen_port;
	uint16_t keepalive;
	bool ok;
};

// Runs on the tcpip thread: everything that takes the lwIP core lock
// (netif_add → wireguardif_init with its raw udp_new/udp_bind, netif_set_up,
// wireguardif_add_peer/connect, netif_set_default).
void begin_core_locked(void* arg) {
	BeginCoreArgs* a = static_cast<BeginCoreArgs*>(arg);
	a->ok = false;

	struct wireguardif_init_data wg;
	struct wireguardif_peer peer;
	wg.private_key = a->private_key;
	wg.listen_port = a->listen_port;
	wg.bind_netif = NULL;

	wireguardif_peer_init(&peer);
	peer.endpoint_ip = a->endpoint_ip;
	peer.public_key = a->peer_public_key;
	peer.preshared_key = NULL;
	peer.keep_alive = a->keepalive;
	// Allow all IPs through tunnel
	peer.allowed_ip = IPADDR4_INIT_BYTES(0, 0, 0, 0);
	peer.allowed_mask = IPADDR4_INIT_BYTES(0, 0, 0, 0);
	peer.endport_port = a->listen_port;

	// Register the new WireGuard network interface with lwIP
	wg_netif = netif_add(&wg_netif_struct, ip_2_ip4(&a->ipaddr), ip_2_ip4(&a->netmask), ip_2_ip4(&a->gateway),
	                     &wg, &wireguardif_init, &ip_input);
	if (wg_netif == NULL) {
		log_e(TAG "failed to initialize WG netif.");
		return;
	}
	// Mark the interface as administratively up, link up flag is set
	// automatically when peer connects
	netif_set_up(wg_netif);

	// Register the new WireGuard peer with the network interface. Upstream
	// ignored the result; on failure the init-allocated device/udp state is
	// closed here so a retry starts clean.
	if (wireguardif_add_peer(wg_netif, &peer, &wireguard_peer_index) != ERR_OK ||
	    wireguard_peer_index == WIREGUARDIF_INVALID_INDEX) {
		log_e(TAG "failed to add WG peer.");
		wireguardif_shutdown(wg_netif);
		netif_remove(wg_netif);
		wg_netif = NULL;
		return;
	}

	if (!ip_addr_isany(&peer.endpoint_ip)) {
		// Start outbound connection to peer
		log_i(TAG "connecting wireguard...");
		wireguardif_connect(wg_netif, wireguard_peer_index);
		// Save the current default interface for restoring when shutting
		// down the WG interface.
		previous_default_netif = netif_default;
		// Set default interface to WG device.
		netif_set_default(wg_netif);
	}

	a->ok = true;
}

void end_core_locked(void* /*arg*/) {
	// Restore the default interface.
	netif_set_default(previous_default_netif);
	previous_default_netif = nullptr;
	// Disconnect the WG interface.
	wireguardif_disconnect(wg_netif, wireguard_peer_index);
	// Remove peer from the WG interface
	wireguardif_remove_peer(wg_netif, wireguard_peer_index);
	wireguard_peer_index = WIREGUARDIF_INVALID_INDEX;
	// Shutdown the wireguard interface.
	wireguardif_shutdown(wg_netif);
	// Remove the WG interface;
	netif_remove(wg_netif);
	wg_netif = nullptr;
}

// One queued tcpip-thread call with its completion signal.
struct CoreCall {
	tcpip_callback_fn fn;
	void* user;
	SemaphoreHandle_t done;
};

void core_trampoline(void* p) {
	CoreCall* call = static_cast<CoreCall*>(p);
	call->fn(call->user);
	xSemaphoreGive(call->done);
}

// Run fn on the tcpip thread and wait for completion. The handshake work
// initiation and DH precompute briefly run on the tcpip task's stack — the
// same as the responder-side handshake rx path.
bool run_on_tcpip_thread(tcpip_callback_fn fn, void* arg) {
	// Heap-allocated: if the wait below ever timed out (tcpip thread wedged —
	// a device already past the point of recovery), a still-queued callback
	// must not read freed memory; the call is then deliberately leaked rather
	// than reused.
	CoreCall* call = new (std::nothrow) CoreCall();
	if (call == NULL) {
		return false;
	}
	call->fn = fn;
	call->user = arg;
	call->done = xSemaphoreCreateBinary();
	if (call->done == NULL) {
		delete call;
		return false;
	}
	if (tcpip_callback(core_trampoline, call) != ERR_OK) {
		vSemaphoreDelete(call->done);
		delete call;
		return false;
	}
	const bool completed = xSemaphoreTake(call->done, pdMS_TO_TICKS(10000)) == pdTRUE;
	if (completed) {
		vSemaphoreDelete(call->done);
		delete call;
	}
	return completed;
}

} // namespace

bool WireGuard::begin(const IPAddress& localIP, const char* privateKey, const char* remotePeerAddress, const char* remotePeerPublicKey, uint16_t remotePeerPort, uint16_t keepaliveSeconds) {
	assert(privateKey != NULL);
	assert(remotePeerAddress != NULL);
	assert(remotePeerPublicKey != NULL);
	assert(remotePeerPort != 0);

	if (_is_initialized) {
		log_e(TAG "begin() called while already initialized.");
		return false;
	}

	// Resolve the endpoint on the CALLER's task: lwip_getaddrinfo blocks (the
	// retries below add up to ~10 s) and would stall the whole network stack
	// on the tcpip thread.
	BeginCoreArgs args;
	memset(&args, 0, sizeof(args));
	args.ipaddr = IPADDR4_INIT(static_cast<uint32_t>(localIP));
	args.netmask = IPADDR4_INIT_BYTES(255, 255, 255, 255);
	args.gateway = IPADDR4_INIT_BYTES(0, 0, 0, 0);
	args.private_key = privateKey;
	args.peer_public_key = remotePeerPublicKey;
	args.listen_port = remotePeerPort;
	args.keepalive = keepaliveSeconds;

	bool success_get_endpoint_ip = false;
	for(int retry = 0; retry < 5; retry++) {
		ip_addr_t endpoint_ip = IPADDR4_INIT_BYTES(0, 0, 0, 0);
		struct addrinfo *res = NULL;
		struct addrinfo hint;
		memset(&hint, 0, sizeof(hint));
		memset(&endpoint_ip, 0, sizeof(endpoint_ip));
		if( lwip_getaddrinfo(remotePeerAddress, NULL, &hint, &res) != 0 ) {
			vTaskDelay(pdMS_TO_TICKS(2000));
			continue;
		}
		success_get_endpoint_ip = true;
		struct in_addr addr4 = ((struct sockaddr_in *) (res->ai_addr))->sin_addr;
		inet_addr_to_ip4addr(ip_2_ip4(&endpoint_ip), &addr4);
		lwip_freeaddrinfo(res);

		args.endpoint_ip = endpoint_ip;
		log_i(TAG "%s is %3d.%3d.%3d.%3d"
			, remotePeerAddress
			, (endpoint_ip.u_addr.ip4.addr >>  0) & 0xff
			, (endpoint_ip.u_addr.ip4.addr >>  8) & 0xff
			, (endpoint_ip.u_addr.ip4.addr >> 16) & 0xff
			, (endpoint_ip.u_addr.ip4.addr >> 24) & 0xff
			);
		break;
	}
	if( !success_get_endpoint_ip  ) {
		log_e(TAG "failed to get endpoint ip.");
		return false;
	}

	// Initialize the platform (mbedTLS entropy; no core lock needed).
	wireguard_platform_init();

	if (!run_on_tcpip_thread(begin_core_locked, &args)) {
		log_e(TAG "begin dispatch to tcpip thread failed or timed out.");
		return false;
	}
	if (!args.ok) {
		return false;
	}

	this->_is_initialized = true;
	return true;
}

bool WireGuard::is_up() const {
	if (!this->_is_initialized || wg_netif == NULL || wireguard_peer_index == WIREGUARDIF_INVALID_INDEX) return false;
	return wireguardif_peer_is_up(wg_netif, wireguard_peer_index, NULL, NULL) == ERR_OK;
}

void WireGuard::end() {
	if( !this->_is_initialized ) return;

	// Teardown touches netif state: run it on the tcpip thread like begin.
	if (!run_on_tcpip_thread(end_core_locked, NULL)) {
		log_e(TAG "end dispatch to tcpip thread failed or timed out; state left as-is.");
		return;
	}

	this->_is_initialized = false;
}
