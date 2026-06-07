/*
 * zenoh_espnow_link.c
 *
 * Implements zenoh-pico's UDP multicast PAL (_z_udp_multicast_*) with ESP-NOW.
 *
 * Transport semantics:
 *   write  -> esp_now_send(FF:FF:FF:FF:FF:FF, buf, len)   // broadcast
 *   read   -> block on FreeRTOS queue fed by recv callback
 *   open   -> esp_now_init() + add broadcast peer
 *   close  -> esp_now_deinit() + delete queue
 */

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "zenoh-pico/link/transport/udp_multicast.h"
#include "zenoh_espnow.h"

static const char *TAG = "zenoh_espnow";

/* ---- Tuning constants ---- */

#define ESPNOW_QUEUE_SIZE 8
#define ESPNOW_PAYLOAD_MAX 250      /* ESP-NOW v1.0 hard limit */
#define ESPNOW_TX_RETRIES 3         /* retries on ESP_ERR_ESPNOW_NO_MEM */
#define ESPNOW_TX_RETRY_DELAY_MS 5  /* delay between retries */
#define ESPNOW_RX_DROP_LOG_EVERY 10 /* log a warning every N drops */

/* ---- Internal state ---- */

typedef struct {
    uint8_t src_mac[ESP_NOW_ETH_ALEN];
    uint8_t data[ESPNOW_PAYLOAD_MAX];
    uint16_t len;
} _espnow_rx_item_t;

static QueueHandle_t s_rx_queue = NULL;
static TickType_t s_rx_ticks = portMAX_DELAY;
static bool s_initialized = false;
static uint32_t s_rx_dropped = 0;
static uint32_t s_tx_failed = 0;

static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ---- Callbacks ---- */

static void _espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (s_rx_queue == NULL || len <= 0 || len > ESPNOW_PAYLOAD_MAX) {
        s_rx_dropped++;
        return;
    }
    _espnow_rx_item_t item;
    memcpy(item.src_mac, recv_info->src_addr, ESP_NOW_ETH_ALEN);
    memcpy(item.data, data, (size_t)len);
    item.len = (uint16_t)len;

    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
        s_rx_dropped++;
        if ((s_rx_dropped % ESPNOW_RX_DROP_LOG_EVERY) == 1) {
            ESP_LOGW(TAG, "RX queue full, dropped=%" PRIu32, s_rx_dropped);
        }
    }
}

static void _espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        s_tx_failed++;
        ESP_LOGD(TAG, "TX fail to " MACSTR " (total=%" PRIu32 ")", MAC2STR(tx_info->des_addr), s_tx_failed);
    }
}

/* ---- Init / deinit ---- */

static esp_err_t _espnow_init(void) {
    if (s_initialized) return ESP_OK;

    s_rx_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(_espnow_rx_item_t));
    if (s_rx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create RX queue (heap exhausted?)");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s  (is Wi-Fi started?)", esp_err_to_name(err));
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        return err;
    }

    esp_now_register_recv_cb(_espnow_recv_cb);
    esp_now_register_send_cb(_espnow_send_cb);

    esp_now_peer_info_t peer = {
        .channel = 0,
        .ifidx = ESP_IF_WIFI_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, BROADCAST_MAC, ESP_NOW_ETH_ALEN);
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "esp_now_add_peer failed: %s", esp_err_to_name(err));
        esp_now_deinit();
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        return err;
    }

    s_initialized = true;
    uint8_t mac[6];
    uint8_t ch;
    wifi_second_chan_t sc;
    esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
    esp_wifi_get_channel(&ch, &sc);
    ESP_LOGI(TAG, "Ready  MAC=" MACSTR "  ch=%d", MAC2STR(mac), ch);
    return ESP_OK;
}

/* ---- zenoh-pico UDP multicast PAL ---- */

z_result_t _z_udp_multicast_endpoint_init_from_address(_z_sys_net_endpoint_t *ep, const _z_string_t *address) {
    return _z_udp_multicast_default_endpoint_init_from_address(ep, address);
}

void _z_udp_multicast_endpoint_clear(_z_sys_net_endpoint_t *ep) { _z_udp_multicast_default_endpoint_clear(ep); }

z_result_t _z_udp_multicast_open(_z_sys_net_socket_t *sock, const _z_sys_net_endpoint_t rep, _z_sys_net_endpoint_t *lep,
                                 uint32_t tout, const char *iface) {
    (void)rep;
    (void)tout;
    (void)iface;
    if (_espnow_init() != ESP_OK) return _Z_ERR_GENERIC;
    if (lep != NULL) lep->_iptcp = NULL;
    sock->_fd = 0;
    return _Z_RES_OK;
}

z_result_t _z_udp_multicast_listen(_z_sys_net_socket_t *sock, const _z_sys_net_endpoint_t rep, uint32_t tout,
                                   const char *iface, const char *join) {
    (void)rep;
    (void)iface;
    (void)join;
    if (_espnow_init() != ESP_OK) return _Z_ERR_GENERIC;
    s_rx_ticks = (tout == 0) ? portMAX_DELAY : pdMS_TO_TICKS(tout);
    sock->_fd = 0;
    return _Z_RES_OK;
}

void _z_udp_multicast_close(_z_sys_net_socket_t *sockrecv, _z_sys_net_socket_t *socksend,
                            const _z_sys_net_endpoint_t rep, const _z_sys_net_endpoint_t lep) {
    (void)socksend;
    (void)rep;
    (void)lep;
    if (!s_initialized) return;

    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_del_peer(BROADCAST_MAC);
    esp_now_deinit();

    vQueueDelete(s_rx_queue);
    s_rx_queue = NULL;
    s_initialized = false;
    sockrecv->_fd = -1;
    ESP_LOGI(TAG, "Closed  rx_dropped=%" PRIu32 "  tx_failed=%" PRIu32, s_rx_dropped, s_tx_failed);
}

size_t _z_udp_multicast_read(const _z_sys_net_socket_t sock, uint8_t *ptr, size_t len, const _z_sys_net_endpoint_t lep,
                             _z_slice_t *addr) {
    (void)sock;
    (void)lep;
    if (s_rx_queue == NULL) return SIZE_MAX;

    _espnow_rx_item_t item;
    if (xQueueReceive(s_rx_queue, &item, s_rx_ticks) != pdTRUE) {
        return SIZE_MAX;
    }

    size_t n = (item.len < (uint16_t)len) ? item.len : (uint16_t)len;
    memcpy(ptr, item.data, n);

    if (addr != NULL && addr->start != NULL && addr->len >= ESP_NOW_ETH_ALEN) {
        memcpy((uint8_t *)addr->start, item.src_mac, ESP_NOW_ETH_ALEN);
        addr->len = ESP_NOW_ETH_ALEN;
    }

    return n;
}

size_t _z_udp_multicast_read_exact(const _z_sys_net_socket_t sock, uint8_t *ptr, size_t len,
                                   const _z_sys_net_endpoint_t lep, _z_slice_t *addr) {
    return _z_udp_multicast_read(sock, ptr, len, lep, addr);
}

size_t _z_udp_multicast_write(const _z_sys_net_socket_t sock, const uint8_t *ptr, size_t len,
                              const _z_sys_net_endpoint_t rep) {
    (void)sock;
    (void)rep;
    if (len > ESPNOW_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "TX dropped: %zu > %d bytes (too large)", len, ESPNOW_PAYLOAD_MAX);
        s_tx_failed++;
        return SIZE_MAX;
    }

    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < ESPNOW_TX_RETRIES; i++) {
        err = esp_now_send(BROADCAST_MAC, ptr, len);
        if (err == ESP_OK) return len;
        if (err != ESP_ERR_ESPNOW_NO_MEM) break;
        vTaskDelay(pdMS_TO_TICKS(ESPNOW_TX_RETRY_DELAY_MS));
    }

    s_tx_failed++;
    ESP_LOGW(TAG, "TX failed: %s (total=%" PRIu32 ")", esp_err_to_name(err), s_tx_failed);
    return SIZE_MAX;
}

/* ---- Public helpers ---- */

uint8_t zenoh_espnow_get_channel(void) {
    uint8_t ch = 0;
    wifi_second_chan_t sc;
    esp_wifi_get_channel(&ch, &sc);
    return ch;
}

uint32_t zenoh_espnow_get_rx_dropped(void) { return s_rx_dropped; }

uint32_t zenoh_espnow_get_tx_failed(void) { return s_tx_failed; }
