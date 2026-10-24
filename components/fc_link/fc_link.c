#include "fc_link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fc_config_default(fc_config_t *c)
{
    *c = (fc_config_t){ .version=1, .baud=115200, .tx_pin=11, .rx_pin=12,
        .system_id=1, .component_id=242, .fc_component_id=1,
        .target_system=255, .target_component=190, .max_rate=5 };
}

static bool usable_pin(unsigned pin)
{
    // XIAO D0..D10 only. Exclude flash/PSRAM, USB, LED, boot, JTAG and power pins.
    const uint8_t pins[] = {1,0,25,7,23,24,11,12,8,9,10};
    for (size_t i=0; i<sizeof(pins); ++i) if (pin==pins[i]) return true;
    return false;
}

bool fc_config_valid(const fc_config_t *c)
{
    return c && c->version==1 && c->enabled<=1 &&
        (c->baud==57600 || c->baud==115200 || c->baud==230400 || c->baud==460800) &&
        usable_pin(c->tx_pin) && usable_pin(c->rx_pin) && c->tx_pin!=c->rx_pin &&
        c->system_id>0 && c->component_id>0 && c->fc_component_id>0 &&
        c->component_id!=c->fc_component_id && c->max_rate>=1 && c->max_rate<=50 &&
        !(c->target_system==c->system_id && c->target_component==c->component_id) &&
        c->reserved[0]==0 && c->reserved[1]==0;
}

bool fc_config_parse(fc_config_t *out, const char *text)
{
    // Strict decimal tokens; reject signs, overflow, extras and narrowing wraps.
    uint32_t values[10];
    for (unsigned i=0; i<10; ++i) {
        while (*text==' ') ++text;
        if (*text<'0' || *text>'9') return false;
        uint32_t value=0;
        do {
            if (value>1000000) return false;
            value=value*10+(unsigned)(*text++-'0');
        } while (*text>='0' && *text<='9');
        if (*text && *text!=' ') return false;
        values[i]=value;
    }
    while (*text==' ') ++text;
    if (*text) return false;
    for (unsigned i=0; i<10; ++i) if (i!=1 && values[i]>255) return false;
    fc_config_t c={.version=1,.enabled=values[0],.baud=values[1],.tx_pin=values[2],
        .rx_pin=values[3],.system_id=values[4],.component_id=values[5],
        .fc_component_id=values[6],.target_system=values[7],.target_component=values[8],
        .max_rate=values[9]};
    if (!fc_config_valid(&c)) return false;
    *out=c;
    return true;
}

void fc_link_init(fc_link_t *link, const fc_config_t *config, const uint8_t mac[6], uint64_t session)
{
    memset(link,0,sizeof(*link));
    link->config=*config;
    memcpy(link->receiver_mac,mac,6);
    link->session=session;
}

static void put(uint8_t *dst, uint64_t value, unsigned bytes)
{
    for (unsigned i=0; i<bytes; ++i) dst[i]=(uint8_t)(value>>(8*i));
}

static void header(fc_link_t *link, uint8_t payload[128], uint8_t kind,
                   uint32_t sequence, uint64_t received, uint64_t now)
{
    memset(payload,0,128);
    memcpy(payload,"DRID",4); payload[4]=1; payload[5]=kind;
    memcpy(payload+6,link->receiver_mac,6);
    put(payload+12,link->session,8); put(payload+20,sequence,4);
    put(payload+24,received,8); put(payload+32,now,8);
}

static size_t tunnel(fc_link_t *link, const uint8_t payload[128], uint8_t len, uint8_t *out)
{
    mavlink_message_t msg;
    // UNKNOWN (0) + an explicit magic/version, not an unregistered global type.
    mavlink_msg_tunnel_pack_status(link->config.system_id,link->config.component_id,
        &link->tx_status,&msg,link->config.target_system,link->config.target_component,0,len,payload);
    return mavlink_msg_to_send_buffer(out,&msg);
}

size_t fc_heartbeat(fc_link_t *link, uint8_t out[MAVLINK_MAX_PACKET_LEN])
{
    mavlink_message_t msg;
    mavlink_msg_heartbeat_pack_status(link->config.system_id,link->config.component_id,
        &link->tx_status,&msg,MAV_TYPE_ONBOARD_CONTROLLER,MAV_AUTOPILOT_INVALID,0,0,MAV_STATE_ACTIVE);
    return mavlink_msg_to_send_buffer(out,&msg);
}

size_t fc_observation(fc_link_t *link, const rid_observation_t *o, uint64_t now, uint8_t out[MAVLINK_MAX_PACKET_LEN])
{
    uint8_t p[128];
    header(link,p,1,o->seq,o->rx_us,now);
    p[40]=(uint8_t)o->transport; p[41]=(uint8_t)o->rssi;
    memcpy(p+42,o->address,6);
    p[48]=o->transport==RID_BLE_LEGACY ? o->address_type : 255;
    p[49]=o->channel; p[50]=o->payload.counter;
    p[51]=o->pack_index; p[52]=o->pack_size ? o->pack_size : 1;
    memcpy(p+53,o->payload.message,25);
    return tunnel(link,p,FC_OBSERVATION_SIZE,out);
}

size_t fc_health(fc_link_t *link, uint32_t sequence, uint64_t now,
                 const uint32_t counters[11], uint8_t out[MAVLINK_MAX_PACKET_LEN])
{
    uint8_t p[128];
    header(link,p,2,sequence,now,now);
    for (unsigned i=0; i<11; ++i) put(p+40+4*i,counters[i],4);
    return tunnel(link,p,FC_HEALTH_SIZE,out);
}

bool fc_heartbeat_fresh(const fc_telemetry_t *t, uint64_t now)
{
    return t->heartbeat_seen && now>=t->heartbeat_us && now-t->heartbeat_us<FC_STALE_US;
}

void fc_receive(fc_link_t *link, const uint8_t *bytes, size_t len, uint64_t now)
{
    mavlink_message_t msg;
    mavlink_status_t status;
    fc_telemetry_t *t=&link->telemetry;
    for (size_t i=0; i<len; ++i) {
        uint8_t rc=mavlink_frame_char_buffer(&link->rx_message,&link->rx_status,bytes[i],&msg,&status);
        if (rc==MAVLINK_FRAMING_BAD_CRC || rc==MAVLINK_FRAMING_BAD_SIGNATURE) {++t->parse_errors; continue;}
        if (rc!=MAVLINK_FRAMING_OK) continue;
        ++t->rx_frames;
        // Signing/key management is not implemented. Never call a signed frame
        // authenticated merely because its checksum was valid.
        if (msg.incompat_flags & MAVLINK_IFLAG_SIGNED) {++t->signed_ignored; continue;}
        if (msg.sysid!=link->config.system_id || msg.compid!=link->config.fc_component_id) continue;
        if (msg.msgid==MAVLINK_MSG_ID_HEARTBEAT) {
            mavlink_heartbeat_t hb; mavlink_msg_heartbeat_decode(&msg,&hb);
            if (hb.autopilot==MAV_AUTOPILOT_INVALID || hb.type==MAV_TYPE_GCS) continue;
            t->heartbeat_seen=true; t->heartbeat_us=now;
            t->custom_mode=hb.custom_mode; t->base_mode=hb.base_mode;
            t->autopilot=hb.autopilot; t->vehicle_type=hb.type;
        } else if (fc_heartbeat_fresh(t,now) && msg.msgid==MAVLINK_MSG_ID_GLOBAL_POSITION_INT) {
            mavlink_global_position_int_t pos; mavlink_msg_global_position_int_decode(&msg,&pos);
            if (pos.lat < -900000000 || pos.lat > 900000000 || pos.lon < -1800000000 || pos.lon > 1800000000) continue;
            t->position_seen=true; t->position_us=now;
            t->lat=pos.lat; t->lon=pos.lon; t->altitude_mm=pos.alt;
            t->relative_altitude_mm=pos.relative_alt; t->heading_cdeg=pos.hdg;
        } else if (fc_heartbeat_fresh(t,now) && msg.msgid==MAVLINK_MSG_ID_GPS_RAW_INT) {
            mavlink_gps_raw_int_t gps; mavlink_msg_gps_raw_int_decode(&msg,&gps);
            t->gps_seen=true; t->gps_us=now; t->fix_type=gps.fix_type; t->satellites=gps.satellites_visible;
        }
    }
}
