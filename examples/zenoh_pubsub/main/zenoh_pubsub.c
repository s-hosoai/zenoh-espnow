#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "zenoh-pico.h"

static const char *TAG = "zenoh_ps";

/* ---- Wi-Fi ---- */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_MAX_RETRY     5

static EventGroupHandle_t s_wifi_eg;
static int s_retry = 0;
static bool s_wifi_ok = false;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "Wi-Fi retry %d/%d", s_retry, WIFI_MAX_RETRY);
        } else {
            ESP_LOGE(TAG, "Wi-Fi connection failed");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        s_wifi_ok = true;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wcfg = {
        .sta = {
            .ssid     = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

/* ---- Zenoh subscriber callback ---- */
static void sub_handler(z_loaned_sample_t *sample, void *arg)
{
    z_view_string_t key;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &key);

    z_owned_string_t payload;
    z_bytes_to_string(z_sample_payload(sample), &payload);

    ESP_LOGI(TAG, "RX '%.*s': '%.*s'",
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

    ESP_LOGI(TAG, "Connecting to Wi-Fi '%s'...", CONFIG_WIFI_SSID);
    wifi_init_sta();

    /* Zenoh session config */
    z_owned_config_t config;
    z_config_default(&config);

#if CONFIG_ZENOH_MODE == 0
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "client");
    zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, CONFIG_ZENOH_LOCATOR);
    ESP_LOGI(TAG, "Zenoh client → %s", CONFIG_ZENOH_LOCATOR);
#else
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "peer");
    // UDP multicast — iface must match the actual netif name on ESP-IDF (sta)
    zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY,
                     "udp/224.0.0.225:7447#iface=sta");
    ESP_LOGI(TAG, "Zenoh peer (UDP multicast)");
#endif

    z_owned_session_t s;
    ESP_LOGI(TAG, "Opening Zenoh session...");
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

    /* Declare subscriber on demo/greeting */
    z_owned_closure_sample_t cb;
    z_closure(&cb, sub_handler, NULL, NULL);
    z_view_keyexpr_t sub_ke;
    z_view_keyexpr_from_str_unchecked(&sub_ke, "demo/greeting");
    z_owned_subscriber_t sub;
    if (z_declare_subscriber(z_loan(s), &sub, z_loan(sub_ke), z_move(cb), NULL) < 0) {
        ESP_LOGE(TAG, "Failed to declare subscriber");
        z_drop(z_move(s));
        return;
    }
    ESP_LOGI(TAG, "Subscribed to 'demo/greeting'");

    /* Publish every 5 s */
    uint8_t mac[6];
    esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
    char buf[80];
    uint32_t seq = 0;

    z_view_keyexpr_t pub_ke;
    z_view_keyexpr_from_str_unchecked(&pub_ke, "demo/greeting");

    while (1) {
        snprintf(buf, sizeof(buf), "hello from " MACSTR " #%"PRIu32, MAC2STR(mac), seq++);
        z_owned_bytes_t payload;
        z_bytes_copy_from_str(&payload, buf);
        z_put(z_loan(s), z_loan(pub_ke), z_move(payload), NULL);
        ESP_LOGI(TAG, "TX: %s", buf);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    z_drop(z_move(sub));
    z_drop(z_move(s));
}
