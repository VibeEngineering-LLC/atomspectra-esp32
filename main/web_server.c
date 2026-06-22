#include "atomspectra.h"
#include "shproto.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stddef.h>
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_littlefs.h"

static const char *TAG = "web";

#define STORAGE_PATH "/sto" "rage"

static int parse_saved_index(const char *uri)
{
    const char *p = strstr(uri, "/api/saved/");
    if (!p) return -1;
    return atoi(p + 11);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    const spectrum_data_t *sp = spectrum_get_current();
    const device_info_t   *di = spectrum_get_device_info();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "analyzer_connected", usb_host_cdc_is_connected());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());
    cJSON_AddNumberToObject(root, "total_counts", sp->total_counts);
    cJSON_AddNumberToObject(root, "cpu_load", sp->cpu_load);
    cJSON_AddBoolToObject(root, "tcp_client", tcp_bridge_client_connected());

    if (di->valid) {
        cJSON_AddNumberToObject(root, "dev", di->dev);
        cJSON_AddNumberToObject(root, "version", di->version);
        cJSON_AddNumberToObject(root, "mode", di->mode);
        cJSON_AddNumberToObject(root, "freq", di->freq);
        cJSON_AddNumberToObject(root, "t1", di->t1);
        cJSON_AddNumberToObject(root, "t2", di->t2);
        cJSON_AddNumberToObject(root, "t3", di->t3);
        cJSON_AddNumberToObject(root, "time", di->time_sec);
        cJSON_AddNumberToObject(root, "noise", di->noise);
        cJSON_AddNumberToObject(root, "max", di->max_integral);
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_spectrum(httpd_req_t *req)
{
    const spectrum_data_t *sp = spectrum_get_current();
    if (!sp->valid) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data yet");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline");
    httpd_resp_send(req, (const char *)sp->bins, SPECTRUM_CHANNELS * sizeof(uint32_t));
    return ESP_OK;
}

static esp_err_t render_spectrum_json(httpd_req_t *req, const spectrum_data_t *sp)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"bins\":[");
    char num[16];
    for (int i = 0; i < SPECTRUM_CHANNELS; i++) {
        int len = snprintf(num, sizeof(num), "%s%u", (i > 0) ? "," : "", sp->bins[i]);
        httpd_resp_send_chunk(req, num, len);
    }
    char tail[320];
    snprintf(tail, sizeof(tail),
        "],\"total\":%u,\"cpu\":%u,\"cps\":%u,\"lost\":%u,\"time\":%u,"
        "\"t1\":%.1f,\"t2\":%.1f,\"t3\":%.1f,\"serial\":\"%s\"}",
        sp->total_counts, sp->cpu_load, sp->cps, sp->lost_impulses,
        sp->total_time_sec,
        sp->temperature[0], sp->temperature[1], sp->temperature[2],
        sp->serial_number[0] ? sp->serial_number : "");
    httpd_resp_sendstr_chunk(req, tail);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_spectrum_json(httpd_req_t *req)
{
    const spectrum_data_t *sp = spectrum_get_current();
    if (!sp->valid) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data yet");
        return ESP_FAIL;
    }
    return render_spectrum_json(req, sp);
}

static esp_err_t handle_command(httpd_req_t *req)
{
    char body[256] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';
    uint8_t pkt_buf[512];
    shproto_struct pkt;
    shproto_init(&pkt, pkt_buf, sizeof(pkt_buf));
    shproto_packet_start(&pkt, CMD_TEXT);
    for (int i = 0; i <= recv_len; i++)
        shproto_packet_add_data(&pkt, body[i]);
    shproto_packet_complete(&pkt);
    int ret = usb_host_cdc_send(pkt.data, pkt.len);
    httpd_resp_sendstr(req, ret == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t handle_reset(httpd_req_t *req)
{
    uint8_t pkt_buf[64];
    shproto_struct pkt;
    shproto_init(&pkt, pkt_buf, sizeof(pkt_buf));
    shproto_packet_start(&pkt, CMD_TEXT);
    const char *cmd = "-rst";
    for (int i = 0; cmd[i]; i++) shproto_packet_add_data(&pkt, cmd[i]);
    shproto_packet_add_data(&pkt, '\0');
    shproto_packet_complete(&pkt);
    usb_host_cdc_send(pkt.data, pkt.len);
    spectrum_reset();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handle_save(httpd_req_t *req)
{
    int idx = spectrum_save_to_flash();
    char resp[64];
    if (idx >= 0)
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"index\":%d}", idx);
    else
        snprintf(resp, sizeof(resp), "{\"ok\":false}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t handle_list(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"spectra\":[");
    char path[64], item[160];
    int count = 0;
    for (int i = 0; i < 9999; i++) {
        snprintf(path, sizeof(path), "%s/spec_%04d.bin", STORAGE_PATH, i);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint32_t counts = 0, time_sec = 0;
        time_t saved_at = 0;
        fseek(f, offsetof(spectrum_data_t, total_counts), SEEK_SET);
        fread(&counts, 4, 1, f);
        fread(&time_sec, 4, 1, f);
        fseek(f, offsetof(spectrum_data_t, saved_at), SEEK_SET);
        fread(&saved_at, sizeof(time_t), 1, f);
        fclose(f);
        int n = snprintf(item, sizeof(item), "%s{\"index\":%d,\"counts\":%u,\"time\":%u,\"saved_at\":%ld}",
            count > 0 ? "," : "", i, counts, time_sec, (long)saved_at);
        httpd_resp_send_chunk(req, item, n);
        count++;
    }
    char tail[32];
    int n = snprintf(tail, sizeof(tail), "],\"count\":%d}", count);
    httpd_resp_send_chunk(req, tail, n);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_index(httpd_req_t *req)
{
    extern const uint8_t index_html_start[] asm("_binary_index_html_start");
    extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

static esp_err_t render_spectrum_xml(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/xml");
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    httpd_resp_sendstr_chunk(req,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<ResultDataFile xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\""
        " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\r\n"
        "  <FormatVersion>120920</FormatVersion>\r\n"
        "  <ResultDataList>\r\n"
        "    <ResultData>\r\n");

    time_t end_time = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
    struct tm ts, te;
    time_t t_start = end_time - sp->total_time_sec;
    localtime_r(&t_start, &ts);
    localtime_r(&end_time, &te);

    int n = snprintf(buf, 4096,
        "      <SampleInfo>\r\n"
        "        <Name>%s</Name>\r\n"
        "        <Location />\r\n"
        "        <Time>%04d-%02d-%02dT%02d:%02d:%02d</Time>\r\n"
        "        <Weight>1</Weight>\r\n"
        "        <Volume>1</Volume>\r\n"
        "        <Note />\r\n"
        "      </SampleInfo>\r\n"
        "      <DeviceConfigReference>\r\n"
        "        <Name>Atom Spectra</Name>\r\n"
        "        <Guid>00000000-0000-0000-0000-000000000000</Guid>\r\n"
        "      </DeviceConfigReference>\r\n"
        "      <StartTime>%04d-%02d-%02dT%02d:%02d:%02d</StartTime>\r\n"
        "      <EndTime>%04d-%02d-%02dT%02d:%02d:%02d</EndTime>\r\n"
        "      <PresetTime>0</PresetTime>\r\n",
        sp->serial_number[0] ? sp->serial_number : "AtomSpectra",
        ts.tm_year+1900, ts.tm_mon+1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec,
        ts.tm_year+1900, ts.tm_mon+1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec,
        te.tm_year+1900, te.tm_mon+1, te.tm_mday, te.tm_hour, te.tm_min, te.tm_sec);
    httpd_resp_send_chunk(req, buf, n);

    n = snprintf(buf, 4096,
        "      <EnergySpectrum>\r\n"
        "        <NumberOfChannels>%d</NumberOfChannels>\r\n"
        "        <ChannelPitch>1</ChannelPitch>\r\n",
        SPECTRUM_CHANNELS);
    httpd_resp_send_chunk(req, buf, n);

    if (sp->calib_valid) {
        n = snprintf(buf, 4096,
            "        <EnergyCalibration>\r\n"
            "          <PolynomialOrder>%d</PolynomialOrder>\r\n"
            "          <Coefficients>\r\n",
            sp->calib_order);
        httpd_resp_send_chunk(req, buf, n);
        for (int i = 0; i <= sp->calib_order; i++) {
            n = snprintf(buf, 4096,
                "            <Coefficient>%.15g</Coefficient>\r\n",
                sp->calibration[i]);
            httpd_resp_send_chunk(req, buf, n);
        }
        httpd_resp_sendstr_chunk(req,
            "          </Coefficients>\r\n"
            "        </EnergyCalibration>\r\n");
    }

    float live_time = (float)sp->total_time_sec;
    if (sp->cpu_load > 0 && sp->cpu_load < 10000)
        live_time *= (1.0f - (float)sp->cpu_load / 10000.0f);

    n = snprintf(buf, 4096,
        "        <ValidPulseCount>%u</ValidPulseCount>\r\n"
        "        <TotalPulseCount>%u</TotalPulseCount>\r\n"
        "        <MeasurementTime>%u</MeasurementTime>\r\n"
        "        <LiveTime>%.1f</LiveTime>\r\n"
        "        <NumberOfSamples>1</NumberOfSamples>\r\n"
        "        <Spectrum>\r\n",
        sp->total_counts,
        sp->total_counts + sp->lost_impulses,
        sp->total_time_sec, live_time);
    httpd_resp_send_chunk(req, buf, n);

    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int pos = 0;
        for (int j = 0; j < 80 && i < SPECTRUM_CHANNELS; j++, i++) {
            pos += snprintf(buf + pos, 4096 - pos,
                "          <DataPoint>%u</DataPoint>\r\n", sp->bins[i]);
        }
        httpd_resp_send_chunk(req, buf, pos);
    }

    httpd_resp_sendstr_chunk(req,
        "        </Spectrum>\r\n"
        "      </EnergySpectrum>\r\n"
        "      <BackgroundEnergySpectrum>\r\n"
        "        <NumberOfChannels>0</NumberOfChannels>\r\n"
        "        <ChannelPitch>1</ChannelPitch>\r\n"
        "        <MeasurementTime>0</MeasurementTime>\r\n"
        "        <NumberOfSamples>0</NumberOfSamples>\r\n"
        "        <Spectrum />\r\n"
        "      </BackgroundEnergySpectrum>\r\n"
        "      <PulseCollection>\r\n"
        "        <Format>Base64 encoded binary</Format>\r\n"
        "        <Pulses />\r\n"
        "      </PulseCollection>\r\n"
        "    </ResultData>\r\n"
        "  </ResultDataList>\r\n"
        "</ResultDataFile>\r\n");

    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

static esp_err_t render_spectrum_csv(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/csv");
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    if (sp->calib_valid) {
        int pos = snprintf(buf, 4096, "calibcoeff:");
        for (int i = 0; i <= sp->calib_order; i++)
            pos += snprintf(buf + pos, 4096 - pos, " %.15g", sp->calibration[i]);
        pos += snprintf(buf + pos, 4096 - pos, "\n");
        httpd_resp_send_chunk(req, buf, pos);
    }

    int n = snprintf(buf, 4096, "remark: %s\n",
        sp->serial_number[0] ? sp->serial_number : "AtomSpectra");
    httpd_resp_send_chunk(req, buf, n);

    float live_time = (float)sp->total_time_sec;
    if (sp->cpu_load > 0 && sp->cpu_load < 10000)
        live_time *= (1.0f - (float)sp->cpu_load / 10000.0f);

    time_t end_time = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
    struct tm ts;
    time_t t_start = end_time - sp->total_time_sec;
    localtime_r(&t_start, &ts);

    n = snprintf(buf, 4096,
        "livetime: %.1f\n"
        "realtime: %u\n"
        "detectorname: Atom Spectra\n"
        "SerialNumber: %s\n"
        "starttime: %04d-%02d-%02dT%02d:%02d:%02d\n",
        live_time, sp->total_time_sec,
        sp->serial_number[0] ? sp->serial_number : "unknown",
        ts.tm_year+1900, ts.tm_mon+1, ts.tm_mday,
        ts.tm_hour, ts.tm_min, ts.tm_sec);
    httpd_resp_send_chunk(req, buf, n);

    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int pos = 0;
        for (int j = 0; j < 200 && i < SPECTRUM_CHANNELS; j++, i++) {
            pos += snprintf(buf + pos, 4096 - pos, "%d, %u\n", i + 1, sp->bins[i]);
        }
        httpd_resp_send_chunk(req, buf, pos);
    }

    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_export_xml(httpd_req_t *req)
{
    const spectrum_data_t *sp = spectrum_get_current();
    if (!sp->valid) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    return render_spectrum_xml(req, sp, "spectrum.xml");
}

static esp_err_t handle_export_csv(httpd_req_t *req)
{
    const spectrum_data_t *sp = spectrum_get_current();
    if (!sp->valid) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    return render_spectrum_csv(req, sp, "spectrum.csv");
}

static esp_err_t handle_saved_export_xml(httpd_req_t *req)
{
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    spectrum_data_t *sp = malloc(sizeof(spectrum_data_t));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (spectrum_load_from_flash(idx, sp) != 0) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Spectrum not found");
        return ESP_FAIL;
    }
    char fn[32];
    snprintf(fn, sizeof(fn), "spectrum_%04d.xml", idx);
    esp_err_t ret = render_spectrum_xml(req, sp, fn);
    free(sp);
    return ret;
}

static esp_err_t handle_saved_export_csv(httpd_req_t *req)
{
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    spectrum_data_t *sp = malloc(sizeof(spectrum_data_t));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (spectrum_load_from_flash(idx, sp) != 0) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Spectrum not found");
        return ESP_FAIL;
    }
    char fn[32];
    snprintf(fn, sizeof(fn), "spectrum_%04d.csv", idx);
    esp_err_t ret = render_spectrum_csv(req, sp, fn);
    free(sp);
    return ret;
}

static esp_err_t handle_saved_json(httpd_req_t *req)
{
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    spectrum_data_t *sp = malloc(sizeof(spectrum_data_t));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (spectrum_load_from_flash(idx, sp) != 0) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Spectrum not found");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_json(req, sp);
    free(sp);
    return ret;
}

static esp_err_t handle_saved_delete(httpd_req_t *req)
{
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    if (spectrum_delete_from_flash(idx) == 0)
        httpd_resp_sendstr(req, "{\"ok\":true}");
    else
        httpd_resp_sendstr(req, "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t handle_device(httpd_req_t *req)
{
    const device_info_t *di = spectrum_get_device_info();
    const spectrum_data_t *sp = spectrum_get_current();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "valid", di->valid);
    if (di->valid) {
        cJSON_AddNumberToObject(root, "dev", di->dev);
        cJSON_AddNumberToObject(root, "version", di->version);
        cJSON_AddNumberToObject(root, "rise", di->rise);
        cJSON_AddNumberToObject(root, "fall", di->fall);
        cJSON_AddNumberToObject(root, "noise", di->noise);
        cJSON_AddNumberToObject(root, "freq", di->freq);
        cJSON_AddNumberToObject(root, "max_integral", di->max_integral);
        cJSON_AddNumberToObject(root, "hyst", di->hyst);
        cJSON_AddNumberToObject(root, "mode", di->mode);
        cJSON_AddNumberToObject(root, "step", di->step);
        cJSON_AddNumberToObject(root, "time", di->time_sec);
        cJSON_AddNumberToObject(root, "pot", di->pot);
        cJSON_AddNumberToObject(root, "t1", di->t1);
        cJSON_AddNumberToObject(root, "t2", di->t2);
        cJSON_AddNumberToObject(root, "t3", di->t3);
        cJSON_AddBoolToObject(root, "tc_on", di->tc_on);
        cJSON_AddNumberToObject(root, "tp", di->tp);
    }
    if (sp->serial_number[0])
        cJSON_AddStringToObject(root, "serial", sp->serial_number);
    if (sp->calib_valid) {
        cJSON *cal = cJSON_CreateArray();
        for (int i = 0; i <= sp->calib_order; i++)
            cJSON_AddItemToArray(cal, cJSON_CreateNumber(sp->calibration[i]));
        cJSON_AddItemToObject(root, "calibration", cal);
        cJSON_AddNumberToObject(root, "calib_order", sp->calib_order);
    }
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_reboot_device(httpd_req_t *req)
{
    uint8_t pkt_buf[64];
    shproto_struct pkt;
    shproto_init(&pkt, pkt_buf, sizeof(pkt_buf));
    shproto_packet_start(&pkt, CMD_REBOOT);
    shproto_packet_complete(&pkt);
    int ret = usb_host_cdc_send(pkt.data, pkt.len);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ret == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t handle_system(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_sec", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(root, "usb_connected", usb_host_cdc_is_connected());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());
    cJSON_AddBoolToObject(root, "tcp_client", tcp_bridge_client_connected());
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    cJSON_AddNumberToObject(root, "flash_total", (double)total);
    cJSON_AddNumberToObject(root, "flash_used", (double)used);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(root, "rssi", ap.rssi);
        cJSON_AddStringToObject(root, "ssid", (char *)ap.ssid);
    }
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_wifi_reset(httpd_req_t *req)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

void web_server_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    const httpd_uri_t uris[] = {
        {"/api/status",                  HTTP_GET,  handle_status,           NULL},
        {"/api/spectrum",                HTTP_GET,  handle_spectrum,         NULL},
        {"/api/spectrum.json",           HTTP_GET,  handle_spectrum_json,    NULL},
        {"/api/command",                 HTTP_POST, handle_command,          NULL},
        {"/api/reset",                   HTTP_POST, handle_reset,            NULL},
        {"/api/save",                    HTTP_POST, handle_save,             NULL},
        {"/api/list",                    HTTP_GET,  handle_list,             NULL},
        {"/api/export.xml",              HTTP_GET,  handle_export_xml,       NULL},
        {"/api/export.csv",              HTTP_GET,  handle_export_csv,       NULL},
        {"/api/saved/*/export.xml",      HTTP_GET,  handle_saved_export_xml, NULL},
        {"/api/saved/*/export.csv",      HTTP_GET,  handle_saved_export_csv, NULL},
        {"/api/saved/*/spectrum.json",   HTTP_GET,  handle_saved_json,       NULL},
        {"/api/saved/*/delete",          HTTP_POST, handle_saved_delete,     NULL},
        {"/api/device",                  HTTP_GET,  handle_device,           NULL},
        {"/api/system",                  HTTP_GET,  handle_system,           NULL},
        {"/api/reboot-device",           HTTP_POST, handle_reboot_device,    NULL},
        {"/api/wifi/reset",              HTTP_POST, handle_wifi_reset,       NULL},
        {"/",                            HTTP_GET,  handle_index,            NULL},
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
}