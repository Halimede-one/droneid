#include "rid.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int rid_parse_legacy(const uint8_t *adv, size_t len, rid_payload_t *out)
{
    if (!adv || !out) return -1;
    bool found = false;
    rid_payload_t candidate = {0};
    for (size_t offset = 0; offset < len;) {
        size_t field_len = adv[offset++];
        if (field_len == 0) break;
        if (field_len > len - offset) return -1;
        const uint8_t *field = adv + offset;
        if (field_len >= 4 && field[0] == 0x16 && field[1] == 0xfa &&
            field[2] == 0xff && field[3] == 0x0d) {
            if (field_len != 30 || found) return -1;
            candidate.counter = field[4];
            memcpy(candidate.message, field + 5, ODID_MESSAGE_SIZE);
            found = true;
        }
        offset += field_len;
    }
    if (found) *out = candidate;
    return found ? 1 : 0;
}

void rid_identity_default(rid_identity_t *identity, const uint8_t mac[6])
{
    memset(identity, 0, sizeof(*identity));
    snprintf(identity->id, sizeof(identity->id), "BENCH-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(identity->hello, sizeof(identity->hello), "BENCH TEST - NO FLIGHT");
}

bool rid_identity_valid(const rid_identity_t *identity)
{
    if (!identity || !identity->id[0]) return false;
    size_t i;
    for (i = 0; i < sizeof(identity->id) && identity->id[i]; ++i) {
        char c = identity->id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    if (i > RID_BENCH_ID_MAX) return false;
    for (i = 0; i < sizeof(identity->hello) && identity->hello[i]; ++i)
        if ((unsigned char)identity->hello[i] < 0x20 || (unsigned char)identity->hello[i] > 0x7e) return false;
    return i <= RID_HELLO_MAX;
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool unhex(char *out, size_t capacity, const char *hex)
{
    size_t len = strlen(hex);
    if (len % 2 || len / 2 >= capacity) return false;
    for (size_t i = 0; i < len; i += 2) {
        int a = nibble(hex[i]), b = nibble(hex[i + 1]);
        if (a < 0 || b < 0 || (a == 0 && b == 0)) return false;
        out[i / 2] = (char)((a << 4) | b);
    }
    out[len / 2] = 0;
    return true;
}

bool rid_identity_from_hex(rid_identity_t *out, const char *id_hex, const char *hello_hex)
{
    if (!out || !id_hex || !hello_hex) return false;
    rid_identity_t value = {0};
    if (!unhex(value.id, sizeof(value.id), id_hex) ||
        (strcmp(hello_hex, "-") && !unhex(value.hello, sizeof(value.hello), hello_hex)) ||
        !rid_identity_valid(&value)) return false;
    *out = value;
    return true;
}

bool rid_identity_advertisement(const rid_identity_t *identity, bool self_id,
                                uint8_t counter, uint8_t out[RID_ADV_SIZE])
{
    if (!out || !rid_identity_valid(identity)) return false;
    ODID_Message_encoded encoded = {0};
    int rc;
    if (self_id) {
        ODID_SelfID_data data;
        odid_initSelfIDData(&data);
        data.DescType = ODID_DESC_TYPE_TEXT;
        memcpy(data.Desc, identity->hello, sizeof(identity->hello));
        rc = encodeSelfIDMessage(&encoded.selfId, &data);
    } else {
        ODID_BasicID_data data;
        odid_initBasicIDData(&data);
        data.UAType = ODID_UATYPE_NONE;
        data.IDType = ODID_IDTYPE_SPECIFIC_SESSION_ID;
        // Session types 225..255 are reserved for private experiments.
        data.UASID[0] = (char)0xff;
        memcpy(data.UASID + 1, identity->id, strlen(identity->id));
        rc = encodeBasicIDMessage(&encoded.basicId, &data);
    }
    if (rc != ODID_SUCCESS) return false;
    const uint8_t prefix[] = {30, 0x16, 0xfa, 0xff, 0x0d};
    memcpy(out, prefix, sizeof(prefix));
    out[5] = counter;
    memcpy(out + 6, encoded.rawData, ODID_MESSAGE_SIZE);
    return true;
}

bool rid_bench_advertisement(const uint8_t mac[6], bool self_id,
                             uint8_t counter, uint8_t out[RID_ADV_SIZE])
{
    if (!mac) return false;
    rid_identity_t identity;
    rid_identity_default(&identity, mac);
    return rid_identity_advertisement(&identity, self_id, counter, out);
}

typedef struct {
    char *data;
    size_t capacity;
    size_t used;
    bool failed;
} json_buffer_t;

static void append(json_buffer_t *b, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void append(json_buffer_t *b, const char *format, ...)
{
    if (b->failed) return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(b->data + b->used, b->capacity - b->used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= b->capacity - b->used) {
        b->failed = true;
        return;
    }
    b->used += (size_t)n;
}

static void string(json_buffer_t *b, const char *s, size_t max_len)
{
    append(b, "\"");
    for (size_t i = 0; i < max_len && s[i]; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') append(b, "\\%c", c);
        else if (c < 0x20 || c >= 0x7f) append(b, "\\u%04x", c);
        else append(b, "%c", c);
    }
    append(b, "\"");
}

static void hex(json_buffer_t *b, const uint8_t *bytes, size_t len)
{
    append(b, "\"");
    for (size_t i = 0; i < len; ++i) append(b, "%02x", bytes[i]);
    append(b, "\"");
}

static void number(json_buffer_t *b, const char *key, double value, bool valid)
{
    append(b, ",\"%s\":", key);
    if (valid && isfinite(value)) append(b, "%.7f", value);
    else append(b, "null");
}

static const char *type_name(unsigned type)
{
    static const char *names[] = {
        "basic_id", "location", "authentication", "self_id", "system", "operator_id"
    };
    return type < 6 ? names[type] : "unsupported";
}

static void decoded(json_buffer_t *b, const uint8_t raw[ODID_MESSAGE_SIZE])
{
    ODID_Message_encoded encoded;
    memcpy(encoded.rawData, raw, ODID_MESSAGE_SIZE);
    unsigned type = raw[0] >> 4;
    if ((raw[0] & 15) > ODID_PROTOCOL_VERSION || type > 5) {
        append(b, ",\"decode_status\":\"unsupported\",\"data\":null");
        return;
    }

    // Use a temporary buffer: invalid decodes never leak partial data objects.
    char fields[1300];
    json_buffer_t f = {.data = fields, .capacity = sizeof(fields)};
    int rc = ODID_FAIL;
    switch (type) {
    case ODID_MESSAGETYPE_BASIC_ID: {
        ODID_BasicID_data data = {0};
        rc = decodeBasicIDMessage(&data, &encoded.basicId);
        append(&f, "\"id_type\":%u,\"ua_type\":%u,\"uas_id_hex\":",
               (unsigned)data.IDType, (unsigned)data.UAType);
        // Preserve exactly the 20 on-air bytes, including binary identifiers.
        hex(&f, raw + 2, ODID_ID_SIZE);
        append(&f, ",\"uas_id_text\":");
        if (data.IDType == ODID_IDTYPE_SERIAL_NUMBER ||
            data.IDType == ODID_IDTYPE_CAA_REGISTRATION_ID)
            string(&f, data.UASID, ODID_ID_SIZE);
        else append(&f, "null");
        break;
    }
    case ODID_MESSAGETYPE_LOCATION: {
        ODID_Location_data d = {0};
        rc = decodeLocationMessage(&d, &encoded.location);
        bool coordinates_valid = (d.Latitude != 0 || d.Longitude != 0) &&
            d.Latitude >= -90 && d.Latitude <= 90 &&
            d.Longitude >= -180 && d.Longitude <= 180;
        append(&f, "\"status\":%u,\"height_reference\":%u",
               (unsigned)d.Status, (unsigned)d.HeightType);
        number(&f, "latitude_deg", d.Latitude, coordinates_valid);
        number(&f, "longitude_deg", d.Longitude, coordinates_valid);
        number(&f, "altitude_geodetic_m", d.AltitudeGeo, d.AltitudeGeo != INV_ALT);
        number(&f, "altitude_barometric_m", d.AltitudeBaro, d.AltitudeBaro != INV_ALT);
        number(&f, "height_m", d.Height, d.Height != INV_ALT);
        number(&f, "direction_deg", d.Direction, d.Direction >= 0 && d.Direction <= MAX_DIR);
        number(&f, "horizontal_speed_mps", d.SpeedHorizontal, d.SpeedHorizontal <= MAX_SPEED_H);
        number(&f, "vertical_speed_mps", d.SpeedVertical, d.SpeedVertical >= MIN_SPEED_V && d.SpeedVertical <= MAX_SPEED_V);
        number(&f, "seconds_after_hour", d.TimeStamp, d.TimeStamp < MAX_TIMESTAMP);
        append(&f, ",\"horizontal_accuracy_code\":%u,\"vertical_accuracy_code\":%u,"
               "\"barometric_accuracy_code\":%u,\"speed_accuracy_code\":%u,\"time_accuracy_code\":%u",
               (unsigned)d.HorizAccuracy, (unsigned)d.VertAccuracy,
               (unsigned)d.BaroAccuracy, (unsigned)d.SpeedAccuracy, (unsigned)d.TSAccuracy);
        break;
    }
    case ODID_MESSAGETYPE_SELF_ID: {
        ODID_SelfID_data d = {0};
        rc = decodeSelfIDMessage(&d, &encoded.selfId);
        append(&f, "\"description_type\":%u,\"description\":", (unsigned)d.DescType);
        string(&f, d.Desc, ODID_STR_SIZE);
        break;
    }
    case ODID_MESSAGETYPE_SYSTEM: {
        ODID_System_data d = {0};
        rc = decodeSystemMessage(&d, &encoded.system);
        bool valid = (d.OperatorLatitude != 0 || d.OperatorLongitude != 0) &&
            d.OperatorLatitude >= -90 && d.OperatorLatitude <= 90 &&
            d.OperatorLongitude >= -180 && d.OperatorLongitude <= 180;
        append(&f, "\"operator_location_type\":%u,\"classification_type\":%u",
               (unsigned)d.OperatorLocationType, (unsigned)d.ClassificationType);
        number(&f, "operator_latitude_deg", d.OperatorLatitude, valid);
        number(&f, "operator_longitude_deg", d.OperatorLongitude, valid);
        number(&f, "operator_altitude_geodetic_m", d.OperatorAltitudeGeo, d.OperatorAltitudeGeo != INV_ALT);
        number(&f, "timestamp_since_2019_s", d.Timestamp, d.Timestamp != 0);
        append(&f, ",\"area_count\":%u,\"area_radius_m\":%u,\"category_eu\":%u,\"class_eu\":%u",
               d.AreaCount, d.AreaRadius, (unsigned)d.CategoryEU, (unsigned)d.ClassEU);
        number(&f, "area_ceiling_m", d.AreaCeiling, d.AreaCeiling != INV_ALT);
        number(&f, "area_floor_m", d.AreaFloor, d.AreaFloor != INV_ALT);
        break;
    }
    case ODID_MESSAGETYPE_OPERATOR_ID: {
        ODID_OperatorID_data d = {0};
        rc = decodeOperatorIDMessage(&d, &encoded.operatorId);
        append(&f, "\"operator_id_type\":%u,\"operator_id\":", (unsigned)d.OperatorIdType);
        string(&f, d.OperatorId, ODID_ID_SIZE);
        break;
    }
    case ODID_MESSAGETYPE_AUTH: {
        ODID_Auth_data d = {0};
        rc = decodeAuthMessage(&d, &encoded.auth);
        append(&f, "\"auth_type\":%u,\"page\":%u", (unsigned)d.AuthType, d.DataPage);
        // Raw page bytes are always preserved; signatures are not verified.
        break;
    }
    }
    if (rc == ODID_SUCCESS && !f.failed)
        append(b, ",\"decode_status\":\"ok\",\"data\":{%s}", fields);
    else append(b, ",\"decode_status\":\"invalid\",\"data\":null");
}

size_t rid_observation_json(char *out, size_t capacity,
                            const rid_observation_t *o,
                            const char *receiver_id, const char *session,
                            uint64_t reported_us)
{
    if (!out || capacity == 0 || !o || !receiver_id || !session) return 0;
    json_buffer_t b = {.data = out, .capacity = capacity};
    append(&b, "{\"schema\":1,\"kind\":\"observation\",\"receiver_id\":");
    string(&b, receiver_id, 32);
    append(&b, ",\"session\":");
    string(&b, session, 32);
    append(&b, ",\"seq\":%" PRIu32 ",\"rx_us\":%" PRIu64 ",\"reported_us\":%" PRIu64,
           o->seq, o->rx_us, reported_us);
    const char *transport = o->transport == RID_WIFI_BEACON ? "wifi_beacon" :
                            o->transport == RID_WIFI_NAN ? "wifi_nan" : "ble_legacy";
    append(&b, ",\"transport\":\"%s\",\"phy\":%s,\"channel\":", transport,
           o->transport == RID_BLE_LEGACY ? "\"1M\"" : "null");
    if (o->channel) append(&b, "%u", o->channel);
    else append(&b, "null");
    append(&b, ",\"frequency_mhz\":");
    if (o->channel) append(&b, "%u", o->channel <= 13 ? 2407 + o->channel * 5 : 5000 + o->channel * 5);
    else append(&b, "null");
    append(&b, ",\"pack_index\":%u,\"pack_size\":%u,\"rssi_dbm\":",
           o->pack_index, o->pack_size ? o->pack_size : 1);
    if (o->rssi == 127) append(&b, "null");
    else append(&b, "%d", o->rssi);
    append(&b, ",\"source_address\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
           "\"source_address_type\":",
           o->address[0], o->address[1], o->address[2], o->address[3], o->address[4], o->address[5]);
    if (o->transport == RID_BLE_LEGACY) append(&b, "%u", o->address_type);
    else append(&b, "null");
    append(&b, ",\"message_counter\":%u,\"protocol_version\":%u,"
           "\"message_type\":\"%s\",\"authentication\":\"unverified\",\"raw_message_hex\":",
           o->payload.counter, o->payload.message[0] & 15,
           type_name(o->payload.message[0] >> 4));
    hex(&b, o->payload.message, ODID_MESSAGE_SIZE);
    decoded(&b, o->payload.message);
    append(&b, "}\n");
    if (b.failed) { out[0] = '\0'; return 0; }
    return b.used;
}
