#include "sound_sync_tx.h"
#include "audio_fft.h"
#include "services.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <string.h>

static const char *TAG = "sound_sync";

#define WLED_MDNS_NAME      "wled-86box.local"
#define MIN_SEND_INTERVAL_US 22000

static int s_sock = -1;
static struct sockaddr_in s_dest;
static bool s_ready;
static bool s_dest_resolved;
static uint64_t s_last_send_us;

/* WLED-MM audioSyncPacket layout (v0.14):
 * Offset  Size  Field
 *   0       6   header (magic: 00 00 00 00 00 02)
 *   6       4   sampleRaw (float)
 *  10       4   sampleSmooth (float)
 *  14       1   samplePeak (uint8)
 *  15      16   fftResult[16] (uint8 × 16)
 *  31       4   FFT_Magnitude (float)
 *  35       4   FFT_MajorPeak (float)
 *  39       1   reserved/padding
 *  Total: 40 bytes (not 44 — corrected from plan estimate)
 */
#define PACKET_SIZE_ACTUAL 40

typedef struct __attribute__((packed)) {
    uint8_t  header[6];
    float    sample_raw;
    float    sample_smooth;
    uint8_t  sample_peak;
    uint8_t  fft_result[16];
    float    fft_magnitude;
    float    fft_major_peak;
    uint8_t  reserved;
} wled_audio_sync_packet_t;

_Static_assert(sizeof(wled_audio_sync_packet_t) == PACKET_SIZE_ACTUAL,
               "Sound Sync packet layout mismatch");

bool sound_sync_tx_is_ready(void)
{
    return s_ready && s_dest_resolved;
}

static bool resolve_destination(void)
{
    if (s_dest_resolved) return true;

    services_status_t status;
    if (services_status_get(&status) != ESP_OK || !status.wifi_connected) return false;

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;

    if (getaddrinfo(WLED_MDNS_NAME, NULL, &hints, &res) == 0 && res) {
        struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
        s_dest.sin_family = AF_INET;
        s_dest.sin_port = htons(SOUND_SYNC_PORT);
        s_dest.sin_addr = addr->sin_addr;
        freeaddrinfo(res);
        s_dest_resolved = true;
        ESP_LOGI(TAG, "Resolved %s -> %s:%d",
                 WLED_MDNS_NAME, inet_ntoa(s_dest.sin_addr), SOUND_SYNC_PORT);
        return true;
    }
    if (res) freeaddrinfo(res);

    /* Fallback: broadcast on the Sound Sync port */
    s_dest.sin_family = AF_INET;
    s_dest.sin_port = htons(SOUND_SYNC_PORT);
    s_dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    s_dest_resolved = true;
    ESP_LOGW(TAG, "mDNS resolve failed; broadcasting to port %d", SOUND_SYNC_PORT);
    return true;
}

static void on_fft_result(const audio_fft_result_t *result, void *user)
{
    (void)user;
    if (!s_ready || s_sock < 0) return;

    uint64_t now_us = esp_timer_get_time();
    if (now_us - s_last_send_us < MIN_SEND_INTERVAL_US) return;

    if (!s_dest_resolved && !resolve_destination()) return;

    wled_audio_sync_packet_t pkt = {0};
    pkt.header[4] = 0x00;
    pkt.header[5] = 0x02;
    pkt.sample_raw = result->sample_raw;
    pkt.sample_smooth = result->sample_smooth;
    pkt.sample_peak = result->sample_peak ? 1 : 0;
    memcpy(pkt.fft_result, result->bin, FFT_BINS);
    pkt.fft_magnitude = result->fft_magnitude;
    pkt.fft_major_peak = result->fft_major_peak;

    int sent = sendto(s_sock, &pkt, sizeof(pkt), 0,
                      (struct sockaddr *)&s_dest, sizeof(s_dest));
    if (sent > 0) {
        s_last_send_us = now_us;
    }
}

esp_err_t sound_sync_tx_start(void)
{
    if (s_ready) return ESP_OK;

    services_status_t status;
    if (services_status_get(&status) != ESP_OK || !status.wifi_connected) {
        ESP_LOGW(TAG, "Sound Sync TX deferred: Wi-Fi not connected");
        return ESP_ERR_INVALID_STATE;
    }

    if (!audio_fft_is_ready()) {
        ESP_LOGW(TAG, "Sound Sync TX deferred: audio FFT not ready");
        return ESP_ERR_INVALID_STATE;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        return ESP_FAIL;
    }

    int broadcast = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    esp_err_t err = audio_fft_subscribe(on_fft_result, NULL);
    if (err != ESP_OK) {
        close(s_sock);
        s_sock = -1;
        ESP_LOGE(TAG, "Failed to subscribe to FFT: %s", esp_err_to_name(err));
        return err;
    }

    resolve_destination();

    s_ready = true;
    ESP_LOGI(TAG, "Sound Sync TX ready -> port %d @ ~43 Hz", SOUND_SYNC_PORT);
    return ESP_OK;
}
