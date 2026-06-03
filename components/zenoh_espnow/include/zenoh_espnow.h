#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Return the channel currently used by the ESP-NOW link.
 * Valid after zenoh-pico opens the multicast transport.
 */
uint8_t zenoh_espnow_get_channel(void);

/**
 * Return the number of packets dropped because the receive queue was full.
 */
uint32_t zenoh_espnow_get_rx_dropped(void);

/**
 * Return the number of transmission failures (queue full after retries,
 * or other esp_now_send errors).
 */
uint32_t zenoh_espnow_get_tx_failed(void);

#ifdef __cplusplus
}
#endif
