#include "slave_ota.h"

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SLAVE_OTA_TASK_STACK   8192
#define SLAVE_OTA_TASK_PRIO    3
#define SLAVE_FW_PARTITION     "slave_fw"
#define OTA_CHUNK_SIZE         1400
#define OTA_STARTUP_DELAY_MS   8000
#define OTA_TARGET_MAJOR       2
#define OTA_TARGET_MINOR       12
#define OTA_TARGET_PATCH       8

static const char *TAG = "slave_ota";

static esp_err_t read_fw_version_from_partition(const esp_partition_t *part,
                                                char *ver_str, size_t ver_len,
                                                size_t *fw_size_out)
{
    esp_image_header_t hdr;
    esp_err_t err = esp_partition_read(part, 0, &hdr, sizeof(hdr));
    if (err != ESP_OK) return err;

    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "Bad magic 0x%02x in %s", hdr.magic, part->label);
        return ESP_ERR_INVALID_ARG;
    }

    esp_image_segment_header_t seg;
    err = esp_partition_read(part, sizeof(hdr), &seg, sizeof(seg));
    if (err != ESP_OK) return err;

    esp_app_desc_t desc;
    err = esp_partition_read(part, sizeof(hdr) + sizeof(seg), &desc, sizeof(desc));
    if (err != ESP_OK) return err;

    snprintf(ver_str, ver_len, "%s", desc.version);

    size_t total = sizeof(hdr);
    size_t off = sizeof(hdr);
    for (int i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t s;
        err = esp_partition_read(part, off, &s, sizeof(s));
        if (err != ESP_OK) return err;
        total += sizeof(s) + s.data_len;
        off += sizeof(s) + s.data_len;
    }

    size_t pad = (16 - (total % 16)) % 16;
    total += pad + 1;
    if (hdr.hash_appended) {
        size_t hash_pad = (16 - (total % 16)) % 16;
        total += hash_pad + 32;
    }

    *fw_size_out = total;
    return ESP_OK;
}

static void slave_ota_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(OTA_STARTUP_DELAY_MS));

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, SLAVE_FW_PARTITION);
    if (!part) {
        ESP_LOGW(TAG, "Partition '%s' not found — skipping OTA", SLAVE_FW_PARTITION);
        goto done;
    }

    uint8_t probe[4];
    esp_partition_read(part, 0, probe, sizeof(probe));
    if (probe[0] == 0xFF && probe[1] == 0xFF && probe[2] == 0xFF && probe[3] == 0xFF) {
        ESP_LOGW(TAG, "Partition '%s' is empty — skipping OTA", SLAVE_FW_PARTITION);
        goto done;
    }

    char new_ver[32];
    size_t fw_size = 0;
    if (read_fw_version_from_partition(part, new_ver, sizeof(new_ver), &fw_size) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot parse slave FW image in partition");
        goto done;
    }
    ESP_LOGI(TAG, "Slave FW in partition: version=%s size=%u", new_ver, (unsigned)fw_size);

    esp_hosted_coprocessor_fwver_t cur = {0};
    int ver_ret = esp_hosted_get_coprocessor_fwversion(&cur);
    if (ver_ret == 0) {
        ESP_LOGI(TAG, "Running slave FW: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 cur.major1, cur.minor1, cur.patch1);

        if (cur.major1 == OTA_TARGET_MAJOR &&
            cur.minor1 == OTA_TARGET_MINOR &&
            cur.patch1 == OTA_TARGET_PATCH) {
            ESP_LOGI(TAG, "Slave already at target version — no OTA needed");
            goto done;
        }
    } else {
        ESP_LOGW(TAG, "Cannot query slave version (ret=%d), proceeding with OTA", ver_ret);
    }

    ESP_LOGI(TAG, "Beginning slave OTA (%u bytes)...", (unsigned)fw_size);
    esp_err_t err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        goto done;
    }

    uint8_t *chunk = heap_caps_malloc(OTA_CHUNK_SIZE, MALLOC_CAP_DEFAULT);
    if (!chunk) {
        ESP_LOGE(TAG, "Failed to alloc OTA chunk buffer");
        esp_hosted_slave_ota_end();
        goto done;
    }

    size_t offset = 0;
    uint32_t chunks = 0;
    bool failed = false;
    while (offset < fw_size) {
        size_t to_read = fw_size - offset;
        if (to_read > OTA_CHUNK_SIZE) to_read = OTA_CHUNK_SIZE;

        err = esp_partition_read(part, offset, chunk, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Partition read error at offset %u: %s", (unsigned)offset, esp_err_to_name(err));
            failed = true;
            break;
        }

        err = esp_hosted_slave_ota_write(chunk, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed at chunk %" PRIu32 ": %s", chunks, esp_err_to_name(err));
            failed = true;
            break;
        }

        offset += to_read;
        chunks++;
        if (chunks % 100 == 0) {
            ESP_LOGI(TAG, "OTA progress: %u/%u bytes (%.0f%%)",
                     (unsigned)offset, (unsigned)fw_size,
                     (float)offset * 100.0f / (float)fw_size);
        }
    }

    free(chunk);

    if (failed) {
        esp_hosted_slave_ota_end();
        ESP_LOGE(TAG, "OTA transfer failed");
        goto done;
    }

    err = esp_hosted_slave_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed: %s", esp_err_to_name(err));
        goto done;
    }

    ESP_LOGI(TAG, "OTA transfer complete, activating new firmware...");
    err = esp_hosted_slave_ota_activate();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_activate failed: %s", esp_err_to_name(err));
        goto done;
    }

    ESP_LOGI(TAG, "Slave OTA succeeded! C6 will reboot with v%s", new_ver);

done:
    vTaskDelete(NULL);
}

void slave_ota_init(void)
{
    xTaskCreate(slave_ota_task, "slave_ota", SLAVE_OTA_TASK_STACK, NULL,
                SLAVE_OTA_TASK_PRIO, NULL);
}
