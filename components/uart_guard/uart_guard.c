#include "uart_guard.h"
#include <limits.h>
#include <string.h>

#define US_PER_SECOND UINT64_C(1000000)
#define FRAME_GAP_US UINT64_C(100000)
#define FC_HEARTBEAT_US UINT64_C(5000000)

static void add(uint32_t *counter, size_t amount)
{
    *counter = amount > UINT32_MAX - *counter ? UINT32_MAX : *counter + (uint32_t)amount;
}

static bool safe_id(uint8_t id)
{
    // 0xfd and 0xfe are frame starts. FC-bound frames contain neither after
    // their first byte, so an FC parser restart cannot enter nested payloads.
    return id != 0 && id != 0xfd && id != 0xfe;
}

void guard_config_default(guard_config_t *c)
{
    *c = (guard_config_t){.system_id=1, .sensor_component=242, .guard_component=243,
        .fc_component=1, .target_system=255, .target_component=190};
}

bool guard_config_valid(const guard_config_t *c)
{
    return c && safe_id(c->system_id) && safe_id(c->sensor_component) &&
        safe_id(c->guard_component) && safe_id(c->fc_component) &&
        safe_id(c->target_system) && safe_id(c->target_component) &&
        c->target_system != c->system_id &&
        c->sensor_component != c->guard_component &&
        c->sensor_component != c->fc_component && c->guard_component != c->fc_component;
}

static void budget_init(guard_budget_t *b, uint32_t rate, uint32_t burst, uint64_t now)
{
    *b = (guard_budget_t){.credit=(uint64_t)burst * US_PER_SECOND,
        .updated_us=now, .rate=rate, .burst=burst};
}

static size_t available(guard_budget_t *b, uint64_t now)
{
    if (now < b->updated_us) return 0; // Monotonic time is a platform precondition.
    uint64_t elapsed = now - b->updated_us;
    if (elapsed > US_PER_SECOND) elapsed = US_PER_SECOND;
    uint64_t credit = b->credit + elapsed * b->rate;
    uint64_t cap = (uint64_t)b->burst * US_PER_SECOND;
    b->credit = credit > cap ? cap : credit;
    b->updated_us = now;
    return (size_t)(b->credit / US_PER_SECOND);
}

static void spend(guard_budget_t *b, size_t bytes)
{
    b->credit -= (uint64_t)bytes * US_PER_SECOND;
}

bool guard_init(uart_guard_t *g, const guard_config_t *config, uint64_t now)
{
    memset(g, 0, sizeof(*g));
    if (!guard_config_valid(config)) return false;
    g->config = *config;
    for (unsigned i=0; i<2; ++i) budget_init(&g->input_budget[i], GUARD_INPUT_RATE, GUARD_INPUT_BURST, now);
    budget_init(&g->output_budget[GUARD_FC], GUARD_FC_OUTPUT_RATE, GUARD_OUTPUT_BURST, now);
    budget_init(&g->output_budget[GUARD_SENSOR], GUARD_SENSOR_OUTPUT_RATE, GUARD_OUTPUT_BURST, now);
    g->enabled = true;
    return true;
}

static uint64_t get64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i=0; i<8; ++i) value |= (uint64_t)p[i] << (8*i);
    return value;
}

static bool observation_valid(const uint8_t *p)
{
    if (p[40] > 2 || p[41] == 127 || p[52] < 1 || p[52] > 9 || p[51] >= p[52]) return false;
    if (p[40] == 0) return p[48] <= 3 && p[49] == 0 && p[51] == 0 && p[52] == 1;
    // Current radio implementation only captures these Wi-Fi channels.
    return p[48] == 255 && (p[49] == 6 || p[49] == 149);
}

static void save(uart_guard_t *g, guard_port_t source, guard_pending_t *slot,
                 uint32_t id, const uint8_t *data, uint8_t length, uint64_t now)
{
    if (slot->present) add(&g->counters.replaced[source], 1);
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->data, data, length);
    slot->length = length;
    slot->message_id = id;
    slot->received_us = now;
    slot->present = true;
}

static bool sensor_frame(uart_guard_t *g, const mavlink_message_t *m, uint64_t now)
{
    if (m->sysid != g->config.system_id || m->compid != g->config.sensor_component) return false;
    if (m->msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        if (m->len != MAVLINK_MSG_ID_HEARTBEAT_LEN) return false;
        mavlink_heartbeat_t hb;
        mavlink_msg_heartbeat_decode(m, &hb);
        // No sensor heartbeat fields are forwarded. The guard owns its heartbeat.
        return hb.type == MAV_TYPE_ONBOARD_CONTROLLER && hb.autopilot == MAV_AUTOPILOT_INVALID &&
            hb.custom_mode == 0 && hb.base_mode == 0 && hb.system_status == MAV_STATE_ACTIVE && hb.mavlink_version == 3;
    }
    if (m->msgid != MAVLINK_MSG_ID_TUNNEL || m->len > MAVLINK_MSG_ID_TUNNEL_LEN) return false;
    mavlink_tunnel_t tunnel;
    mavlink_msg_tunnel_decode(m, &tunnel); // Restores MAVLink 2 trailing zeros.
    if (tunnel.payload_type != 0 || tunnel.target_system != g->config.target_system ||
        tunnel.target_component != g->config.target_component) return false;
    uint8_t *p = tunnel.payload;
    unsigned kind = p[5];
    if (memcmp(p, "DRID", 4) || p[4] != 1 || (kind != 1 && kind != 2)) return false;
    uint8_t length = kind == 1 ? 78 : 84;
    if (tunnel.payload_length != length || get64(p+24) > get64(p+32)) return false;
    // Ignore no hidden suffixes: the unused opaque array must be all zero.
    for (unsigned i=length; i<128; ++i) if (p[i]) return false;
    if (kind == 1 && !observation_valid(p)) return false;
    if (kind == 2 && get64(p+24) != get64(p+32)) return false;
    save(g, GUARD_SENSOR, &g->sensor[kind-1], m->msgid, p, length, now);
    return true;
}

static bool controller_frame(uart_guard_t *g, const mavlink_message_t *m, uint64_t now)
{
    if (m->sysid != g->config.system_id || m->compid != g->config.fc_component) return false;
    unsigned index;
    if (m->msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        if (m->len != MAVLINK_MSG_ID_HEARTBEAT_LEN) return false;
        mavlink_heartbeat_t hb;
        mavlink_msg_heartbeat_decode(m, &hb);
        if (hb.type == MAV_TYPE_GCS || hb.autopilot == MAV_AUTOPILOT_INVALID || hb.mavlink_version != 3) return false;
        g->fc_heartbeat_seen = true;
        g->fc_heartbeat_us = now;
        index = 0;
    } else {
        if (!g->fc_heartbeat_seen || now < g->fc_heartbeat_us || now - g->fc_heartbeat_us >= FC_HEARTBEAT_US) return false;
        if (m->msgid == MAVLINK_MSG_ID_GLOBAL_POSITION_INT) {
            if (m->len > MAVLINK_MSG_ID_GLOBAL_POSITION_INT_LEN) return false;
            mavlink_global_position_int_t pos;
            mavlink_msg_global_position_int_decode(m, &pos);
            if (pos.lat < -900000000 || pos.lat > 900000000 || pos.lon < -1800000000 || pos.lon > 1800000000 ||
                (pos.hdg > 35999 && pos.hdg != UINT16_MAX)) return false;
            index = 1;
        } else if (m->msgid == MAVLINK_MSG_ID_GPS_RAW_INT) {
            if (m->len > MAVLINK_MSG_ID_GPS_RAW_INT_LEN) return false;
            mavlink_gps_raw_int_t gps;
            mavlink_msg_gps_raw_int_decode(m, &gps);
            if (gps.fix_type > 8 || gps.lat < -900000000 || gps.lat > 900000000 ||
                gps.lon < -1800000000 || gps.lon > 1800000000) return false;
            index = 2;
        } else return false;
    }
    save(g, GUARD_FC, &g->fc[index], m->msgid, (const uint8_t *)_MAV_PAYLOAD(m), m->len, now);
    return true;
}

void guard_input_fault(uart_guard_t *g, guard_port_t source)
{
    if (source != GUARD_SENSOR && source != GUARD_FC) return;
    memset(&g->parser[source], 0, sizeof(g->parser[source]));
    if (source == GUARD_SENSOR) memset(g->sensor, 0, sizeof(g->sensor));
    else {
        memset(g->fc, 0, sizeof(g->fc));
        g->fc_heartbeat_seen = false;
    }
    add(&g->counters.framing_errors[source], 1);
}

size_t guard_ingest(uart_guard_t *g, guard_port_t source, const uint8_t *input, size_t length, uint64_t now)
{
    if (!g->enabled || (source != GUARD_SENSOR && source != GUARD_FC) || (!input && length)) return 0;
    add(&g->counters.input_bytes[source], length);
    size_t limit = available(&g->input_budget[source], now);
    if (limit > GUARD_INPUT_CHUNK) limit = GUARD_INPUT_CHUNK;
    if (limit > length) limit = length;
    guard_parser_t *p = &g->parser[source];
    if (now < p->last_byte_us || now - p->last_byte_us > FRAME_GAP_US) memset(p, 0, sizeof(*p));
    p->last_byte_us = now;
    for (size_t i=0; i<limit; ++i) {
        mavlink_message_t message;
        mavlink_status_t status;
        uint8_t result = mavlink_frame_char_buffer(&p->frame, &p->status, input[i], &message, &status);
        if (result == MAVLINK_FRAMING_BAD_CRC || result == MAVLINK_FRAMING_BAD_SIGNATURE) {
            add(&g->counters.framing_errors[source], 1);
        } else if (result == MAVLINK_FRAMING_OK) {
            bool accepted = message.magic == MAVLINK_STX && message.incompat_flags == 0 && message.compat_flags == 0;
            if (accepted) accepted = source == GUARD_SENSOR ? sensor_frame(g, &message, now) : controller_frame(g, &message, now);
            add(accepted ? &g->counters.accepted[source] : &g->counters.rejected[source], 1);
        }
    }
    spend(&g->input_budget[source], limit);
    if (limit != length) {
        add(&g->counters.input_dropped[source], length-limit);
        // Never concatenate bytes across a locally discarded fragment.
        memset(p, 0, sizeof(*p));
    }
    return limit;
}

static uint8_t wrap(const guard_pending_t *slot, uint8_t payload[128])
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    memset(payload, 0, 128);
    memcpy(payload, "DRG2", 4);
    unsigned at = 4;
    // Both supported records are divisible by three, so no padding is needed.
    for (unsigned i=0; i<slot->length; i+=3) {
        uint32_t bits = ((uint32_t)slot->data[i] << 16) | ((uint32_t)slot->data[i+1] << 8) | slot->data[i+2];
        payload[at++] = alphabet[(bits >> 18) & 63];
        payload[at++] = alphabet[(bits >> 12) & 63];
        payload[at++] = alphabet[(bits >> 6) & 63];
        payload[at++] = alphabet[bits & 63];
    }
    return (uint8_t)at;
}

static size_t encode_fc(uart_guard_t *g, const guard_pending_t *slot, uint8_t *output)
{
    uint8_t payload[128];
    uint8_t length = slot ? wrap(slot, payload) : 0;
    // Regenerate flags, IDs, sequence, CRC and heartbeat. Skip sequences whose
    // CRC contains another frame-start byte. The bounded search fails closed.
    for (unsigned attempt=0; attempt<256; ++attempt) {
        uint8_t seq = g->sequence[GUARD_FC]++;
        if (seq == 0xfd || seq == 0xfe) continue;
        mavlink_status_t status = {.current_tx_seq=seq};
        mavlink_message_t message;
        if (slot) {
            mavlink_msg_tunnel_pack_status(g->config.system_id, g->config.guard_component, &status, &message,
                g->config.target_system, g->config.target_component, 0, length, payload);
        } else {
            mavlink_msg_heartbeat_pack_status(g->config.system_id, g->config.guard_component, &status, &message,
                MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
        }
        size_t size = mavlink_msg_to_send_buffer(output, &message);
        bool clean = true;
        for (size_t i=1; i<size; ++i) if (output[i] == 0xfd || output[i] == 0xfe) { clean=false; break; }
        if (clean) return size;
    }
    add(&g->counters.encode_errors, 1);
    return 0;
}

static size_t encode_sensor(uart_guard_t *g, const guard_pending_t *slot, uint8_t *output)
{
    mavlink_message_t message = {.msgid=slot->message_id};
    memcpy(_MAV_PAYLOAD_NON_CONST(&message), slot->data, slot->length);
    uint8_t min_len, max_len, crc;
    if (slot->message_id == MAVLINK_MSG_ID_HEARTBEAT) {
        min_len = MAVLINK_MSG_ID_HEARTBEAT_MIN_LEN; max_len = MAVLINK_MSG_ID_HEARTBEAT_LEN; crc = MAVLINK_MSG_ID_HEARTBEAT_CRC;
    } else if (slot->message_id == MAVLINK_MSG_ID_GLOBAL_POSITION_INT) {
        min_len = MAVLINK_MSG_ID_GLOBAL_POSITION_INT_MIN_LEN; max_len = MAVLINK_MSG_ID_GLOBAL_POSITION_INT_LEN; crc = MAVLINK_MSG_ID_GLOBAL_POSITION_INT_CRC;
    } else {
        min_len = MAVLINK_MSG_ID_GPS_RAW_INT_MIN_LEN; max_len = MAVLINK_MSG_ID_GPS_RAW_INT_LEN; crc = MAVLINK_MSG_ID_GPS_RAW_INT_CRC;
    }
    mavlink_status_t status = {.current_tx_seq=g->sequence[GUARD_SENSOR]++};
    mavlink_finalize_message_buffer(&message, g->config.system_id, g->config.fc_component, &status, min_len, max_len, crc);
    return mavlink_msg_to_send_buffer(output, &message);
}

static bool pending(uart_guard_t *g, guard_pending_t *slot, guard_port_t source, uint64_t now)
{
    if (slot->present && (now < slot->received_us || now-slot->received_us >= GUARD_PENDING_US)) {
        slot->present = false;
        add(&g->counters.expired[source], 1);
    }
    return slot->present;
}

size_t guard_poll(uart_guard_t *g, guard_port_t destination, uint64_t now, uint8_t output[MAVLINK_MAX_PACKET_LEN])
{
    if (!g->enabled || !output || (destination != GUARD_SENSOR && destination != GUARD_FC)) return 0;
    size_t credits = available(&g->output_budget[destination], now);
    guard_pending_t *slot = NULL;
    unsigned index = 0;
    uint64_t period;
    uint64_t *next;
    size_t maximum;
    if (destination == GUARD_FC) {
        bool obs = pending(g, &g->sensor[0], GUARD_SENSOR, now);
        bool health = pending(g, &g->sensor[1], GUARD_SENSOR, now);
        next = g->next_fc_us;
        if (now >= next[0]) { period=1000000; maximum=21; }
        else if (health && now >= next[1]) { index=1; slot=&g->sensor[1]; period=2000000; maximum=133; }
        else if (obs && now >= next[2]) { index=2; slot=&g->sensor[0]; period=200000; maximum=125; }
        else return 0;
    } else {
        next = g->next_sensor_us;
        for (index=0; index<3; ++index) {
            if (pending(g, &g->fc[index], GUARD_FC, now) && now >= next[index]) { slot=&g->fc[index]; break; }
        }
        if (!slot) return 0;
        if (index && (!g->fc_heartbeat_seen || now < g->fc_heartbeat_us || now-g->fc_heartbeat_us >= FC_HEARTBEAT_US)) {
            slot->present = false;
            return 0;
        }
        period = index ? 500000 : 1000000;
        maximum = index == 0 ? 21 : index == 1 ? 40 : 64;
    }
    if (credits < maximum) return 0;
    size_t size = destination == GUARD_FC ? encode_fc(g, slot, output) : encode_sensor(g, slot, output);
    next[index] = now > UINT64_MAX-period ? UINT64_MAX : now+period;
    if (slot) slot->present = false;
    if (!size || size > credits) return 0;
    spend(&g->output_budget[destination], size);
    add(&g->counters.output_frames[destination], 1);
    add(&g->counters.output_bytes[destination], size);
    return size;
}
