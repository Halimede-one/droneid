#include "bench.h"
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static bench_radio_stats_t *stats;
static rid_identity_t identity;
static uint8_t wifi_mac[6], counter, active_channel;
static bool listening;

static bool checked(esp_err_t rc)
{
    if (rc != ESP_OK) atomic_fetch_add(&stats->radio_errors, 1);
    return rc == ESP_OK;
}

static void received(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *packet = buffer;
    if (packet->rx_ctrl.rx_state != 0 || packet->rx_ctrl.sig_len < 4) return;
    atomic_fetch_add(&stats->wifi_frames, 1);
    rid_wifi_packet_t decoded;
    // ESP-IDF sig_len includes the FCS; our portable parser excludes it.
    int rc = rid_parse_wifi(packet->payload, packet->rx_ctrl.sig_len - 4, &decoded);
    if (rc < 0) atomic_fetch_add(&stats->malformed, 1);
    if (rc != 1 || !memcmp(decoded.address, wifi_mac, sizeof(wifi_mac))) return;
    rid_observation_t o = {
        .rssi = packet->rx_ctrl.rssi, .rx_us = (uint64_t)esp_timer_get_time(),
        .transport = decoded.transport, .channel = packet->rx_ctrl.channel,
        .pack_size = decoded.count,
    };
    memcpy(o.address, decoded.address, 6);
    for (unsigned i = 0; i < decoded.count; ++i) {
        o.payload = decoded.messages[i];
        o.pack_index = i;
        bench_submit(&o);
    }
}

void bench_wifi_start(const rid_identity_t *selected_identity, bench_radio_stats_t *radio_stats)
{
    stats = radio_stats;
    identity = *selected_identity;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_country_code("US", false));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
    ESP_ERROR_CHECK(esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE));
    active_channel = 6;
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, wifi_mac));
    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(received));
    atomic_store(&stats->wifi_ready, true);
    // No esp_wifi_connect(): passive capture/raw beacons need no network.
}

bool bench_wifi_listen(uint8_t channel)
{
    if (!atomic_load(&stats->wifi_ready)) return false;
    if (listening && (!channel || channel != active_channel)) {
        if (!checked(esp_wifi_set_promiscuous(false))) return false;
        listening = false;
    }
    if (!channel) return true;
    if (channel != active_channel) {
        if (!checked(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE))) return false;
        active_channel = channel;
    }
    if (!listening) {
        if (!checked(esp_wifi_set_promiscuous(true))) return false;
        listening = true;
    }
    return true;
}

bool bench_wifi_transmit(void)
{
    if (active_channel != 6 || !atomic_load(&stats->wifi_ready)) return false;
    uint8_t frame[RID_WIFI_FRAME_SIZE];
    size_t len = rid_identity_wifi_beacon(&identity, wifi_mac, counter,
                                      esp_timer_get_time(), frame, sizeof(frame));
    if (!len) return checked(ESP_ERR_INVALID_SIZE);
    if (!checked(esp_wifi_80211_tx(WIFI_IF_STA, frame, (int)len, true))) return false;
    ++counter;
    atomic_fetch_add(&stats->wifi_tx_frames, 1);
    return true;
}
