#include "atomspectra.h"
#include "esp_log.h"
#include <inttypes.h>
#include "esp_littlefs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "spectrum";
#define STORAGE_PATH "/sto" "rage"

static spectrum_data_t s_spectrum;
static device_info_t   s_device_info;

void spectrum_init(void)
{
    memset(&s_spectrum, 0, sizeof(s_spectrum));
    memset(&s_device_info, 0, sizeof(s_device_info));
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_PATH,
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_littlefs_info("storage", &total, &used);
        ESP_LOGI(TAG, "LittleFS: total=%zu used=%zu free=%zu", total, used, total - used);
    }
}

void spectrum_process_histogram_chunk(const uint8_t *data, size_t len)
{
    if (len < 6) return;
    uint16_t offset = data[0] | (data[1] << 8);
    size_t bin_bytes = len - 2;
    size_t bin_count = bin_bytes / 4;
    for (size_t i = 0; i < bin_count && (offset + i) < SPECTRUM_CHANNELS; i++) {
        size_t idx = 2 + i * 4;
        s_spectrum.bins[offset + i] =
            data[idx] | (data[idx+1] << 8) | (data[idx+2] << 16) | (data[idx+3] << 24);
    }
    uint32_t total = 0;
    for (int i = 0; i < SPECTRUM_CHANNELS; i++) total += s_spectrum.bins[i];
    s_spectrum.total_counts = total;
    s_spectrum.valid = true;
}

void spectrum_process_stat_packet(const uint8_t *data, size_t len)
{
    if (len < 10) return;
    s_spectrum.total_time_sec = data[0] | (data[1]<<8) | (data[2]<<16) | (data[3]<<24);
    s_spectrum.cpu_load = data[4] | (data[5] << 8);
    s_spectrum.cps = data[6] | (data[7]<<8) | (data[8]<<16) | (data[9]<<24);
    if (len >= 14)
        s_spectrum.lost_impulses = data[10] | (data[11]<<8) | (data[12]<<16) | (data[13]<<24);
}
void spectrum_process_info_response(const char *text)
{
    device_info_t *d = &s_device_info;
    memset(d, 0, sizeof(*d));
    char lbuf[48][64];
    int lcount = 0;
    const char *lp = text;
    while (*lp && lcount < 48) {
        int li = 0;
        while (*lp && *lp != '\n' && *lp != '\r' && li < 63)
            lbuf[lcount][li++] = *lp++;
        lbuf[lcount][li] = '\0';
        while (*lp == '\n' || *lp == '\r') lp++;
        lcount++;
    }
    if (lcount >= 40) {
        strncpy(s_spectrum.serial_number, lbuf[39], sizeof(s_spectrum.serial_number) - 1);
        ESP_LOGI(TAG, "Serial: %s", s_spectrum.serial_number);
    }
    ESP_LOGI(TAG, "Info response: %d lines", lcount);
    for (int i = 0; i < lcount && i < 12; i++)
        ESP_LOGI(TAG, "  L[%d]: \"%s\"", i, lbuf[i]);
    if (lcount >= 11) {
        char hcat[256] = {0};
        for (int i = 0; i < 10; i++) strcat(hcat, lbuf[i]);
        uint32_t cc = 0;
        for (int i = 0; hcat[i]; i++) {
            cc ^= (uint8_t)hcat[i];
            for (int j = 0; j < 8; j++) {
                if (cc & 1) cc = (cc >> 1) ^ 0xEDB88320;
                else cc >>= 1;
            }
        }
        uint32_t ce = (uint32_t)strtoul(lbuf[10], NULL, 16);
        if (cc == ce) {
            for (int c = 0; c < CALIB_COEFFS && (c*2+1) < 10; c++) {
                char pair[128];
                snprintf(pair, sizeof(pair), "%s%s", lbuf[c*2], lbuf[c*2+1]);
                uint64_t raw = strtoull(pair, NULL, 16);
                double val;
                memcpy(&val, &raw, sizeof(val));
                s_spectrum.calibration[c] = val;
            }
            int order = CALIB_COEFFS - 1;
            while (order > 0 && s_spectrum.calibration[order] == 0.0) order--;
            s_spectrum.calib_order = order;
            s_spectrum.calib_valid = true;
            ESP_LOGI(TAG, "Calibration OK: order=%d", s_spectrum.calib_order);
        } else {
            ESP_LOGW(TAG, "Calibration CRC mismatch: computed=%08x expected=%08x", (unsigned)cc, (unsigned)ce);
        }
    }
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        char key[32] = {0};
        int ki = 0;
        while (*p && *p != ' ' && *p != '\n' && ki < 30) key[ki++] = *p++;
        key[ki] = 0;
        while (*p == ' ') p++;
        if (strcmp(key, "DEV") == 0) d->dev = atoi(p);
        else if (strcmp(key, "VERSION") == 0) d->version = atoi(p);
        else if (strcmp(key, "RISE") == 0) d->rise = atoi(p);
        else if (strcmp(key, "FALL") == 0) d->fall = atoi(p);
        else if (strcmp(key, "NOISE") == 0) d->noise = atoi(p);
        else if (strcmp(key, "F") == 0) d->freq = atof(p);
        else if (strcmp(key, "MAX") == 0) d->max_integral = atoi(p);
        else if (strcmp(key, "HYST") == 0) d->hyst = atoi(p);
        else if (strcmp(key, "MODE") == 0) d->mode = atoi(p);
        else if (strcmp(key, "STEP") == 0) d->step = atoi(p);
        else if (strcmp(key, "t") == 0) d->time_sec = atoi(p);
        else if (strcmp(key, "POT") == 0) d->pot = atoi(p);
        else if (strcmp(key, "T1") == 0) d->t1 = atof(p);
        else if (strcmp(key, "T2") == 0) d->t2 = atof(p);
        else if (strcmp(key, "T3") == 0) d->t3 = atof(p);
        else if (strcmp(key, "TC") == 0) d->tc_on = (strncmp(p, "ON", 2) == 0);
        else if (strcmp(key, "TP") == 0) d->tp = atoi(p);
        if (*p == '[') { while (*p && *p != ']') p++; if (*p==']') p++; }
        else { while (*p && *p != ' ' && *p != '\n') p++; }
    }
    d->valid = true;
    s_spectrum.temperature[0] = d->t1;
    s_spectrum.temperature[1] = d->t2;
    s_spectrum.temperature[2] = d->t3;
}
void spectrum_reset(void)
{
    memset(s_spectrum.bins, 0, sizeof(s_spectrum.bins));
    s_spectrum.total_counts = 0;
    s_spectrum.total_time_sec = 0;
    s_spectrum.cps = 0;
    s_spectrum.lost_impulses = 0;
    s_spectrum.valid = false;
}

const spectrum_data_t *spectrum_get_current(void) { return &s_spectrum; }
const device_info_t   *spectrum_get_device_info(void) { return &s_device_info; }

int spectrum_save_to_flash(void)
{
    if (!s_spectrum.valid) return -1;
    char path[64];
    int idx = 0;
    FILE *f;
    while (idx < 9999) {
        snprintf(path, sizeof(path), "%s/spec_%04d.bin", STORAGE_PATH, idx);
        f = fopen(path, "r");
        if (!f) break;
        fclose(f);
        idx++;
    }
    s_spectrum.saved_at = time(NULL);
    f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "Cannot create %s", path); return -1; }
    fwrite(&s_spectrum, sizeof(s_spectrum), 1, f);
    fclose(f);
    ESP_LOGI(TAG, "Saved spectrum to %s (%" PRIu32 " counts)", path, s_spectrum.total_counts);
    return idx;
}

int spectrum_list_saved(char *buf, size_t buf_size)
{
    int count = 0;
    size_t pos = 0;
    char path[64];
    for (int i = 0; i < 9999 && pos < buf_size - 64; i++) {
        snprintf(path, sizeof(path), "%s/spec_%04d.bin", STORAGE_PATH, i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        fclose(f);
        pos += snprintf(buf + pos, buf_size - pos, "%04d\n", i);
        count++;
    }
    return count;
}

int spectrum_load_from_flash(int index, spectrum_data_t *out)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/spec_%04d.bin", STORAGE_PATH, index);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t rd = fread(out, 1, sizeof(*out), f);
    fclose(f);
    return (rd == sizeof(*out)) ? 0 : -1;
}

int spectrum_delete_from_flash(int index)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/spec_%04d.bin", STORAGE_PATH, index);
    if (remove(path) != 0) return -1;
    ESP_LOGI(TAG, "Deleted %s", path);
    return 0;
}