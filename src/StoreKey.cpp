#include "StoreKey.h"

#include <esp_mac.h>

namespace storekey {

bool deriveKey(uint8_t out[vault::kKeyLen]) {
  uint8_t mac[vault::kMacLen];
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return false;
  vault::deriveKey(mac, out);
  return true;
}

} // namespace storekey
