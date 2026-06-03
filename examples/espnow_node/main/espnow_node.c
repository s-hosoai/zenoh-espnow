/*
 * espnow_node.c — Phase 2 verification app
 *
 * Uses zenoh-pico in multicast peer mode with the ESP-NOW transport backend.
 * Wi-Fi is started in STA mode but NOT associated with any AP.
 * Channel is fixed manually so all nodes share the same frequency.
 *
 * Each node:
 *   - Subscribes to  CONFIG_ZENOH_KEY_PREFIX/[all]
 *   - Publishes to   CONFIG_ZENOH_KEY_PREFIX/<MAC>/data  every 2 seconds
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "zenoh-pico.h"

static const char *TAG = "espnow_node";

/* ---- Wi-Fi: start STA without connecting to any AP ---- */
static void wifi_init_no_connect(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    uint8_t mac[6];
    esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
    ESP_LOGI(TAG, "Wi-Fi STA started  MAC=" MACSTR "  ch=%d",
             MAC2STR(mac), CONFIG_ESPNOW_CHANNEL);
}

/* ---- Zenoh subscriber callback ---- */
static void sub_handler(z_loaned_sample_t *sample, void *arg)
{
    (void)arg;
    z_view_string_t key;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &key);

    z_owned_string_t payload;
    z_bytes_to_string(z_sample_payload(sample), &payload);

    ESP_LOGI(TAG, "RX  '%.*s'  '%.*s'",
             (int)z_string_len(z_view_string_loan(&key)),
             z_string_data(z_view_string_loan(&key)),
             (int)z_string_len(z_string_loan(&payload)),
             z_string_data(z_string_loan(&payload)));

    z_string_drop(z_string_move(&payload));
}

/* ---- Main ---- */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_no_connect();

    // Zenoh peer mode. The "udp/..." locator is parsed by zenoh-pico, but the
    // actual transport is provided by zenoh_espnow_link.c via ESP-NOW.
    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "peer");
    zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY,
                     "udp/224.0.0.225:7447#iface=sta");

    ESP_LOGI(TAG, "Opening Zenoh session (ESP-NOW transport)...");
    z_owned_session_t s;
    if (z_open(&s, z_move(config), NULL) < 0) {
        ESP_LOGE(TAG, "Failed to open Zenoh session");
        return;
    }

    if (zp_start_read_task(z_loan_mut(s), NULL) < 0 ||
        zp_start_lease_task(z_loan_mut(s), NULL) < 0) {
        ESP_LOGE(TAG, "Failed to start Zenoh background tasks");
        z_drop(z_move(s));
        return;
    }

    // Subscribe: CONFIG_ZENOH_KEY_PREFIX/**
    char sub_key[64];
    snprintf(sub_key, sizeof(sub_key), "%s/**", CONFIG_ZENOH_KEY_PREFIX);

    z_owned_closure_sample_t cb;
    z_closure(&cb, sub_handler, NULL, NULL);
    z_view_keyexpr_t sub_ke;
    z_view_keyexpr_from_str_unchecked(&sub_ke, sub_key);
    z_owned_subscriber_t sub;
    if (z_declare_subscriber(z_loan(s), &sub, z_loan(sub_ke), z_move(cb), NULL) < 0) {
        ESP_LOGE(TAG, "Failed to declare subscriber");
        z_drop(z_move(s));
        return;
    }
    ESP_LOGI(TAG, "Subscribed to '%s'", sub_key);

    /* Publish: CONFIG_ZENOH_KEY_PREFIX/<MAC>/data */
    uint8_t mac[6];
    esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
    char pub_key[64];
    snprintf(pub_key, sizeof(pub_key), "%s/" MACSTR "/data",
             CONFIG_ZENOH_KEY_PREFIX, MAC2STR(mac));
    ESP_LOGI(TAG, "Publishing on '%s' every 2s", pub_key);

    z_view_keyexpr_t pub_ke;
    z_view_keyexpr_from_str_unchecked(&pub_ke, pub_key);

    char buf[80];
    uint32_t seq = 0;
    while (1) {
        snprintf(buf, sizeof(buf), "{\"seq\":%"PRIu32",\"mac\":\""MACSTR"\"}", seq++, MAC2STR(mac));
        z_owned_bytes_t payload;
        z_bytes_copy_from_str(&payload, buf);
        if (z_put(z_loan(s), z_loan(pub_ke), z_move(payload), NULL) < 0) {
            ESP_LOGW(TAG, "z_put failed");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    z_drop(z_move(sub));
    z_drop(z_move(s));
}
