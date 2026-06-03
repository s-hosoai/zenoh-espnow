/*
 * gateway.c  -  Phase 4/5: ESP-NOW <-> Wi-Fi/zenohd bridge
 *
 * session_a  peer mode, ESP-NOW transport  (towards ESP-NOW nodes)
 * session_b  client mode, TCP transport    (towards zenohd Router)
 *
 * Forwarding (non-overlapping key spaces avoid loops):
 *   sub(a, FWD_ESPNOW_TO_WIFI_KEY) -> pub(b)   ESP-NOW -> Wi-Fi
 *   sub(b, FWD_WIFI_TO_ESPNOW_KEY) -> pub(a)   Wi-Fi  -> ESP-NOW
 *
 * Reconnection:
 *   Wi-Fi disconnect  → always retry (no max limit)
 *   IP regained       → close and reopen session_b with exponential backoff
 *   session_a (ESP-NOW) continues uninterrupted during Wi-Fi outage
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "zenoh-pico.h"
#include "zenoh_espnow.h"

static const char *TAG = "gateway";

/* ---- Global sessions and subscriber ---- */
static z_owned_session_t    s_sa;     /* session_a: ESP-NOW (permanent) */
static z_owned_session_t    s_sb;     /* session_b: TCP/zenohd (reconnectable) */
static z_owned_subscriber_t s_sub_b;  /* subscriber on session_b */

/* Guards fwd_a_to_b from calling z_put on a closed session_b.
 * Set true only after sub_b is declared; cleared before closing session_b.
 * A narrow race window exists (flag check to z_put), but zenoh-pico returns
 * an error rather than crashing on a closed session, so this is acceptable. */
static volatile bool s_session_b_ready = false;

/* ---- Wi-Fi state ---- */
#define WIFI_CONNECTED_BIT    BIT0
#define SESSION_B_RESET_BIT   BIT1  /* set when session_b should be reconnected */

static EventGroupHandle_t s_state_eg;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_state_eg, WIFI_CONNECTED_BIT);
        esp_wifi_connect();  /* retry indefinitely — no max limit for gateway */
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying...");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        uint8_t ch; wifi_second_chan_t sc;
        esp_wifi_get_channel(&ch, &sc);
        ESP_LOGI(TAG, "IP: " IPSTR "  ESP-NOW ch=%d", IP2STR(&ev->ip_info.ip), ch);
        xEventGroupSetBits(s_state_eg, WIFI_CONNECTED_BIT | SESSION_B_RESET_BIT);
    }
}

static void wifi_init_apsta(void)
{
    s_state_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t sta_cfg = {
        .sta = {
            .ssid      = CONFIG_WIFI_SSID,
            .password  = CONFIG_WIFI_PASSWORD,
            .threshold = { .authmode = WIFI_AUTH_WPA2_PSK },
        },
    };
    /* AP: minimal softAP to advertise the channel to ESP-NOW nodes */
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = "gw-espnow",
            .ssid_len       = 10,
            .channel        = 0,    /* follows STA channel after connect */
            .authmode       = WIFI_AUTH_OPEN,
            .max_connection = 0,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,  &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    xEventGroupWaitBits(s_state_eg, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

/* ---- Forwarding helpers ---- */

static void forward_sample(z_loaned_sample_t *sample,
                            z_loaned_session_t *dst,
                            const char *direction)
{
    z_view_string_t key_view;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &key_view);

    ESP_LOGD(TAG, "%s  '%.*s'", direction,
             (int)z_string_len(z_view_string_loan(&key_view)),
             z_string_data(z_view_string_loan(&key_view)));

    z_owned_string_t payload_str;
    z_bytes_to_string(z_sample_payload(sample), &payload_str);

    z_owned_bytes_t fwd;
    z_bytes_copy_from_str(&fwd, z_string_data(z_string_loan(&payload_str)));
    if (z_put(dst, z_sample_keyexpr(sample), z_move(fwd), NULL) < 0) {
        ESP_LOGW(TAG, "%s put failed for '%.*s'", direction,
                 (int)z_string_len(z_view_string_loan(&key_view)),
                 z_string_data(z_view_string_loan(&key_view)));
    }

    z_string_drop(z_string_move(&payload_str));
}

/* sub_a callback: ESP-NOW -> Wi-Fi */
static void fwd_a_to_b(z_loaned_sample_t *sample, void *arg)
{
    (void)arg;
    if (!s_session_b_ready) return;  /* session_b disconnected */
    forward_sample(sample, z_loan_mut(s_sb), "ESP-NOW->WiFi");
}

/* sub_b callback: Wi-Fi -> ESP-NOW */
static void fwd_b_to_a(z_loaned_sample_t *sample, void *arg)
{
    (void)arg;
    forward_sample(sample, z_loan_mut(s_sa), "WiFi->ESP-NOW");
}

/* ---- Session_a (permanent, opened once) ---- */

static z_result_t open_session_a(void)
{
    z_owned_config_t cfg;
    z_config_default(&cfg);
    zp_config_insert(z_loan_mut(cfg), Z_CONFIG_MODE_KEY, "peer");
    zp_config_insert(z_loan_mut(cfg), Z_CONFIG_LISTEN_KEY,
                     "udp/224.0.0.225:7447#iface=sta");

    ESP_LOGI(TAG, "Opening session_a (ESP-NOW)...");
    if (z_open(&s_sa, z_move(cfg), NULL) < 0) {
        ESP_LOGE(TAG, "session_a open failed");
        return _Z_ERR_GENERIC;
    }

    z_task_attr_t attr = { .name = "zr_sa", .priority = 5, .stack_depth = 8192 };
    zp_task_read_options_t  ro = { .task_attributes = &attr };
    zp_task_lease_options_t lo = { .task_attributes = &attr };
    if (zp_start_read_task(z_loan_mut(s_sa), &ro) < 0 ||
        zp_start_lease_task(z_loan_mut(s_sa), &lo) < 0) {
        ESP_LOGE(TAG, "session_a tasks failed");
        return _Z_ERR_GENERIC;
    }
    ESP_LOGI(TAG, "session_a OK");
    return _Z_RES_OK;
}

/* ---- Session_b (reconnectable) ---- */

static void close_session_b(void)
{
    s_session_b_ready = false;       /* stop fwd_a_to_b from using session_b */
    vTaskDelay(pdMS_TO_TICKS(20));   /* let any in-flight fwd_a_to_b call finish */

    z_drop(z_move(s_sub_b));         /* undeclare subscriber first */
    z_drop(z_move(s_sb));            /* close session (stops read/lease tasks) */
    ESP_LOGI(TAG, "session_b closed");
}

static z_result_t open_session_b(void)
{
    z_owned_config_t cfg;
    z_config_default(&cfg);
    zp_config_insert(z_loan_mut(cfg), Z_CONFIG_MODE_KEY, "client");
    zp_config_insert(z_loan_mut(cfg), Z_CONFIG_CONNECT_KEY, CONFIG_ZENOHD_LOCATOR);

    ESP_LOGI(TAG, "Opening session_b (TCP -> %s)...", CONFIG_ZENOHD_LOCATOR);
    if (z_open(&s_sb, z_move(cfg), NULL) < 0) {
        ESP_LOGE(TAG, "session_b open failed");
        return _Z_ERR_GENERIC;
    }

    z_task_attr_t attr = { .name = "zr_sb", .priority = 5, .stack_depth = 8192 };
    zp_task_read_options_t  ro = { .task_attributes = &attr };
    zp_task_lease_options_t lo = { .task_attributes = &attr };
    if (zp_start_read_task(z_loan_mut(s_sb), &ro) < 0 ||
        zp_start_lease_task(z_loan_mut(s_sb), &lo) < 0) {
        ESP_LOGE(TAG, "session_b tasks failed");
        z_drop(z_move(s_sb));
        return _Z_ERR_GENERIC;
    }

    /* Declare sub_b: Wi-Fi -> ESP-NOW forwarding */
    z_owned_closure_sample_t cb;
    z_closure(&cb, fwd_b_to_a, NULL, NULL);
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str_unchecked(&ke, CONFIG_FWD_WIFI_TO_ESPNOW_KEY);
    if (z_declare_subscriber(z_loan(s_sb), &s_sub_b, z_loan(ke), z_move(cb), NULL) < 0) {
        ESP_LOGE(TAG, "sub_b declare failed");
        z_drop(z_move(s_sb));
        return _Z_ERR_GENERIC;
    }

    s_session_b_ready = true;        /* enable ESP-NOW -> Wi-Fi forwarding */
    ESP_LOGI(TAG, "session_b OK  sub('%s')", CONFIG_FWD_WIFI_TO_ESPNOW_KEY);
    return _Z_RES_OK;
}

/* Reconnect session_b with exponential backoff.
 * Blocks until the connection succeeds. */
static void reconnect_session_b(void)
{
    close_session_b();

    uint32_t delay_ms = 1000;
    int attempt = 1;
    while (open_session_b() != _Z_RES_OK) {
        ESP_LOGW(TAG, "session_b reconnect #%d failed, retry in %"PRIu32"ms",
                 attempt++, delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        if (delay_ms < 30000) delay_ms *= 2;  /* cap at 30 s */
    }
}

/* ---- app_main ---- */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_apsta();

    if (open_session_a() != _Z_RES_OK) return;

    /* sub_a: ESP-NOW -> Wi-Fi forwarding (permanent, on session_a) */
    z_owned_closure_sample_t cb_a;
    z_closure(&cb_a, fwd_a_to_b, NULL, NULL);
    z_view_keyexpr_t ke_a;
    z_view_keyexpr_from_str_unchecked(&ke_a, CONFIG_FWD_ESPNOW_TO_WIFI_KEY);
    z_owned_subscriber_t sub_a;
    if (z_declare_subscriber(z_loan(s_sa), &sub_a, z_loan(ke_a), z_move(cb_a), NULL) < 0) {
        ESP_LOGE(TAG, "sub_a declare failed");
        return;
    }
    ESP_LOGI(TAG, "sub_a declared on '%s'", CONFIG_FWD_ESPNOW_TO_WIFI_KEY);

    /* Initial session_b connection */
    reconnect_session_b();

    ESP_LOGI(TAG, "Gateway running");
    ESP_LOGI(TAG, "  ESP-NOW -> Wi-Fi : '%s' -> zenohd", CONFIG_FWD_ESPNOW_TO_WIFI_KEY);
    ESP_LOGI(TAG, "  Wi-Fi  -> ESP-NOW: '%s' -> ESP-NOW", CONFIG_FWD_WIFI_TO_ESPNOW_KEY);

    /* Main loop: heartbeat + session_b reconnect on Wi-Fi recovery */
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            s_state_eg,
            SESSION_B_RESET_BIT,
            pdTRUE,    /* clear on exit */
            pdFALSE,
            pdMS_TO_TICKS(5000));

        if (bits & SESSION_B_RESET_BIT) {
            ESP_LOGI(TAG, "Wi-Fi recovered, reconnecting session_b...");
            /* Wait for IP (already have it since we got the bit on IP_EVENT) */
            reconnect_session_b();
        }

        ESP_LOGI(TAG, "alive  session_b=%s  rx_dropped=%"PRIu32"  tx_failed=%"PRIu32,
                 s_session_b_ready ? "UP" : "DOWN",
                 zenoh_espnow_get_rx_dropped(),
                 zenoh_espnow_get_tx_failed());
    }
}
