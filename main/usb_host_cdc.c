#include "atomspectra.h"
#include "shproto.h"
#include "esp_log.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "usb_cdc";

static cdc_acm_dev_hdl_t s_cdc_dev = NULL;
static SemaphoreHandle_t s_tx_mutex = NULL;

static uint8_t s_rx_buf[1024];
static shproto_struct s_rx_packet;

static uint8_t s_tx_buf[256];
static shproto_struct s_tx_packet;

static usb_raw_rx_cb_t s_raw_rx_cb = NULL;

static void handle_rx_packet(void)
{
    switch (s_rx_packet.cmd) {
    case CMD_HISTOGRAM:
        spectrum_process_histogram_chunk(s_rx_packet.data, s_rx_packet.len);
        break;
    case CMD_TEXT:
        if (s_rx_packet.len > 0) {
            s_rx_packet.data[s_rx_packet.len] = '\0';
            const char *txt = (const char *)s_rx_packet.data;
            ESP_LOGI(TAG, "Text: %s", txt);
            if (strstr(txt, "DEV ") && strstr(txt, "VERSION ")) {
                spectrum_process_info_response(txt);
            }
        }
        break;
    case CMD_STAT:
        spectrum_process_stat_packet(s_rx_packet.data, s_rx_packet.len);
        break;
    case CMD_OSCILLOSCOPE:
        break;
    default:
        ESP_LOGW(TAG, "Unknown cmd 0x%02x len=%u", s_rx_packet.cmd, s_rx_packet.len);
        break;
    }
}

static bool handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    if (s_raw_rx_cb) s_raw_rx_cb(data, data_len);

    for (size_t i = 0; i < data_len; i++) {
        shproto_byte_received(&s_rx_packet, data[i]);
        if (s_rx_packet.ready) {
            s_rx_packet.ready = false;
            handle_rx_packet();
        }
    }
    return true;
}

static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC error");
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "Device disconnected");
        if (s_cdc_dev) {
            cdc_acm_host_close(s_cdc_dev);
            s_cdc_dev = NULL;
        }
        break;
    default:
        break;
    }
}

static void usb_host_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void try_open_device(void)
{
    if (s_cdc_dev) return;

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 1024,
        .in_buffer_size = 1024,
        .event_cb = handle_event,
        .data_cb = handle_rx,
        .user_arg = NULL,
    };

    esp_err_t err = cdc_acm_host_open_vendor_specific(ANALYZER_VID, ANALYZER_PID, 0, &dev_config, &s_cdc_dev);
    if (err != ESP_OK) {
        return;
    }

    cdc_acm_line_coding_t line_coding = {
        .dwDTERate   = ANALYZER_BAUD,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits   = 8,
    };
    cdc_acm_host_line_coding_set(s_cdc_dev, &line_coding);

    ESP_LOGI(TAG, "Analyzer connected (VID=%04x PID=%04x baud=%u)", ANALYZER_VID, ANALYZER_PID, ANALYZER_BAUD);

    // Request device info
    shproto_init(&s_tx_packet, s_tx_buf, sizeof(s_tx_buf));
    shproto_packet_start(&s_tx_packet, CMD_TEXT);
    const char *cmd = "-inf\0";
    for (int i = 0; cmd[i] || i == 4; i++) {
        shproto_packet_add_data(&s_tx_packet, cmd[i]);
        if (cmd[i] == '\0') break;
    }
    shproto_packet_add_data(&s_tx_packet, '\0');
    shproto_packet_complete(&s_tx_packet);
    cdc_acm_host_data_tx_blocking(s_cdc_dev, s_tx_packet.data, s_tx_packet.len, 1000);
}

static void usb_connect_task(void *arg)
{
    while (1) {
        try_open_device();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void usb_host_cdc_init(void)
{
    shproto_init(&s_rx_packet, s_rx_buf, sizeof(s_rx_buf));
    shproto_init(&s_tx_packet, s_tx_buf, sizeof(s_tx_buf));
    s_tx_mutex = xSemaphoreCreateMutex();

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    usb_host_install(&host_config);

    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_lib", 4096, NULL, 2, NULL, 0);

    const cdc_acm_host_driver_config_t driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 3,
        .xCoreID = 0,
    };
    cdc_acm_host_install(&driver_config);

    xTaskCreatePinnedToCore(usb_connect_task, "usb_conn", 4096, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "USB Host CDC initialized, waiting for analyzer...");
}

bool usb_host_cdc_is_connected(void)
{
    return s_cdc_dev != NULL;
}

int usb_host_cdc_send(const uint8_t *data, size_t len)
{
    if (!s_cdc_dev) return -1;
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return -1;
    esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc_dev, data, len, 1000);
    xSemaphoreGive(s_tx_mutex);
    return (err == ESP_OK) ? 0 : -1;
}

void usb_host_cdc_set_raw_rx_cb(usb_raw_rx_cb_t cb)
{
    s_raw_rx_cb = cb;
}

int usb_host_send_text_command(const char *cmd)
{
    uint8_t pkt_buf[256];
    shproto_struct pkt;
    shproto_init(&pkt, pkt_buf, sizeof(pkt_buf));
    shproto_packet_start(&pkt, CMD_TEXT);
    while (*cmd) shproto_packet_add_data(&pkt, *cmd++);
    shproto_packet_add_data(&pkt, '\0');
    shproto_packet_complete(&pkt);
    return usb_host_cdc_send(pkt.data, pkt.len);
}
