#include "sound_sync_tx.h"
#include "audio_fft.h"
#include "services.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <string.h>

static const char *TAG = "sound_sync";

#define WLED_MM_MULTICAST_ADDR_HOST_ORDER 0xEF000001u /* 239.0.0.1 */
#define MIN_SEND_INTERVAL_US 22000

static int s_sock = -1;
static struct sockaddr_in s_dest;
static bool s_ready;
static bool s_dest_configured;
static uint64_t s_last_send_us;
static uint8_t s_frame_counter;

/* WLED-MM audioSyncPacket layout (v2):
 * Offset  Size  Field
 *   0       6   header (ASCII "00002" plus NUL)
 *   6       2   pressure fixed point (optional)
 *   8       4   sampleRaw (float)
 *  12       4   sampleSmooth (float)
 *  16       1   samplePeak (uint8)
 *  17       1   frameCounter (uint8)
 *  18      16   fftResult[16] (uint8)
 *  34       2   zeroCrossingCount (uint16)
 *  36       4   FFT_Magnitude (float)
 *  40       4   FFT_MajorPeak (float)
 */
#define PACKET_SIZE_ACTUAL 44

typedef struct __attribute__((packed)) {
    char     header[6];
    uint8_t  pressure[2];
    float    sample_raw;
    float    sample_smooth;
    uint8_t  sample_peak;
    uint8_t  frame_counter;
    uint8_t  fft_result[16];
    uint16_t zero_crossing_count;
    float    fft_magnitude;
    float    fft_major_peak;
} wled_audio_sync_packet_t;

_Static_assert(sizeof(wled_audio_sync_packet_t) == PACKET_SIZE_ACTUAL,
               "Sound Sync packet layout mismatch");

bool sound_sync_tx_is_ready(void)
{
    return s_ready && s_dest_configured;
}

static void configure_destination(void)
{
    if (s_dest_configured) return;

    s_dest.sin_family = AF_INET;
    s_dest.sin_port = htons(SOUND_SYNC_PORT);
    s_dest.sin_addr.s_addr = htonl(WLED_MM_MULTICAST_ADDR_HOST_ORDER);
    s_dest_configured = true;
    ESP_LOGI(TAG, "Sound Sync target %s:%d", inet_ntoa(s_dest.sin_addr), SOUND_SYNC_PORT);
}

static void on_fft_result(const audio_fft_result_t *result, void *user)
{
    (void)user;
    if (!s_ready || s_sock < 0) return;
    if (services_network_bulk_active()) return;

    uint64_t now_us = esp_timer_get_time();
    if (now_us - s_last_send_us < MIN_SEND_INTERVAL_US) return;

    if (!s_dest_configured) configure_destination();

    wled_audio_sync_packet_t pkt = {0};
    memcpy(pkt.header, "00002", sizeof(pkt.header));
    pkt.sample_raw = result->sample_raw;
    pkt.sample_smooth = result->sample_smooth;
    pkt.sample_peak = result->sample_peak ? 1 : 0;
    pkt.frame_counter = ++s_frame_counter;
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

    if (!audio_fft_is_ready()) {
        ESP_LOGW(TAG, "Sound Sync TX deferred: audio FFT not ready");
        return ESP_ERR_INVALID_STATE;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        return ESP_FAIL;
    }

    uint8_t multicast_ttl = 1;
    setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_TTL, &multicast_ttl, sizeof(multicast_ttl));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    esp_err_t err = audio_fft_subscribe(on_fft_result, NULL);
    if (err != ESP_OK) {
        close(s_sock);
        s_sock = -1;
        ESP_LOGE(TAG, "Failed to subscribe to FFT: %s", esp_err_to_name(err));
        return err;
    }

    configure_destination();

    s_ready = true;
    ESP_LOGI(TAG, "Sound Sync TX ready -> %s:%d @ ~43 Hz",
             inet_ntoa(s_dest.sin_addr), SOUND_SYNC_PORT);
    return ESP_OK;
}
