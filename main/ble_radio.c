#include "bench.h"
#include <string.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static rid_identity_t identity;
static uint8_t own_addr_type, message_counters[2];
static bench_radio_stats_t *stats;

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC) {
        atomic_fetch_add(&stats->advertisements, 1);
        rid_observation_t o = {.transport = RID_BLE_LEGACY, .pack_size = 1};
        int result = rid_parse_legacy(event->disc.data, event->disc.length_data, &o.payload);
        if (result < 0) atomic_fetch_add(&stats->malformed, 1);
        if (result != 1) return 0;
        o.rx_us = (uint64_t)esp_timer_get_time();
        o.rssi = event->disc.rssi;
        o.address_type = event->disc.addr.type;
        for (size_t i = 0; i < 6; ++i) o.address[i] = event->disc.addr.val[5 - i];
        bench_submit(&o);
    }
    return 0;
}

static bool checked(int rc)
{
    if (rc) atomic_fetch_add(&stats->radio_errors, 1);
    return rc == 0;
}

bool bench_ble_scan(bool enable)
{
    if (!atomic_load(&stats->ble_ready)) return !enable;
    if (!enable) return checked(ble_gap_disc_active() ? ble_gap_disc_cancel() : 0);
    if (ble_gap_disc_active()) return true;
    const struct ble_gap_disc_params params = {
        .itvl = 160, .window = 160, // Continuous within this scheduled BLE window.
        .passive = 1, .filter_duplicates = 0,
    };
    return checked(ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL));
}

bool bench_ble_advertise(bool enable, bool self_id)
{
    if (!atomic_load(&stats->ble_ready)) return !enable;
    // Called only by the radio worker, below the NimBLE host task's priority.
    int rc = ble_gap_adv_active() ? ble_gap_adv_stop() : 0;
    if (rc == BLE_HS_EALREADY) rc = 0;
    if (rc || !enable) return checked(rc);
    uint8_t adv[RID_ADV_SIZE];
    unsigned index = self_id ? 1 : 0;
    if (!rid_identity_advertisement(&identity, self_id, message_counters[index], adv))
        return checked(BLE_HS_EINVAL);
    const struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON, .disc_mode = BLE_GAP_DISC_MODE_NON,
        .itvl_min = 160, .itvl_max = 160,
    };
    rc = ble_gap_adv_set_data(adv, sizeof(adv));
    if (rc == 0)
        rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (!checked(rc)) return false;
    atomic_fetch_add(&stats->tx_sets, 1);
    ++message_counters[index];
    return true;
}

static void on_reset(int reason)
{
    (void)reason;
    atomic_store(&stats->ble_ready, false);
    atomic_fetch_add(&stats->radio_errors, 1);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (checked(rc)) atomic_store(&stats->ble_ready, true);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void bench_ble_start(const rid_identity_t *selected_identity, bench_radio_stats_t *radio_stats)
{
    identity = *selected_identity;
    stats = radio_stats;
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
}
