#include "atomspectra.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static void init_sntp(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

void app_main(void)
{
    ESP_LOGI(TAG, "AtomSpectra Gateway starting...");

    wifi_manager_init();

    if (wifi_manager_is_ap_mode()) {
        ESP_LOGI(TAG, "Captive portal active, waiting for WiFi config");
        while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
    }

    spectrum_init();
    usb_host_cdc_init();
    web_server_init();
    tcp_bridge_init();
    init_sntp();

    ESP_LOGI(TAG, "All subsystems initialized");

    int info_tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        const spectrum_data_t *sp = spectrum_get_current();
        ESP_LOGI(TAG, "USB:%s WiFi:%s TCP:%s counts:%u cpu:%u%%",
            usb_host_cdc_is_connected() ? "OK" : "--",
            wifi_is_connected() ? "OK" : "--",
            tcp_bridge_client_connected() ? "OK" : "--",
            sp->total_counts, sp->cpu_load);

        if (usb_host_cdc_is_connected() && ++info_tick >= 3) {
            info_tick = 0;
            usb_host_send_text_command("-inf");
        }
    }
}