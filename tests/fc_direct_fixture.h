#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "fc_link.h"
#ifdef __cplusplus
}
#endif
#include <assert.h>
#include <string.h>

// Use the actual radio encoder. Include legal 0xfd/0xfe bytes in its header,
// metadata and raw RID, including a complete nested SET_MODE frame.
static inline size_t direct_observation(uint8_t *out, bool zero_tail)
{
    fc_config_t config;
    fc_config_default(&config);
    fc_link_t radio;
    const uint8_t mac[6] = {0xfd, 0xfe, 3, 4, 5, 6};
    fc_link_init(&radio, &config, mac, 0xfdfe);
    radio.tx_status.current_tx_seq = 0xfd;
    rid_observation_t observation;
    memset(&observation, 0, sizeof(observation));
    observation.rx_us = 0xfdfe;
    observation.rssi = -2;
    observation.pack_size = 1;
    if (!zero_tail) {
        uint8_t nested[MAVLINK_MAX_PACKET_LEN];
        mavlink_message_t msg;
        mavlink_status_t tx;
        memset(&tx, 0, sizeof(tx));
        mavlink_msg_set_mode_pack_status(1, 242, &tx, &msg, 1, MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, 17);
        unsigned length = mavlink_msg_to_send_buffer(nested, &msg);
        assert(length <= 25);
        memset(observation.payload.message, 0xfe, 25);
        memcpy(observation.payload.message, nested, length);
    }
    return fc_observation(&radio, &observation, 0xfdff, out);
}
