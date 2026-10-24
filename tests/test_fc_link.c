#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fc_link.h"
#include "mavlink_fixtures.h"

static const uint8_t mac[6]={0x10,0xbd,0xa3,0xca,0x38,0x60};
static void fixture(const char *kind, const uint8_t *bytes, size_t len)
{
    printf("{\"kind\":\"%s\",\"frame_hex\":\"",kind);
    for (size_t i=0;i<len;++i) printf("%02x",bytes[i]);
    puts("\"}");
}

int main(void)
{
    fc_config_t config;
    fc_config_default(&config); assert(fc_config_valid(&config)); assert(!config.enabled);
    assert(fc_config_parse(&config,"1 115200 11 12 1 242 1 255 190 5"));
    const char *invalid[]={"2 115200 11 12 1 242 1 255 190 5","1 9600 11 12 1 242 1 255 190 5",
        "1 115200 11 11 1 242 1 255 190 5","1 115200 27 12 1 242 1 255 190 5",
        "1 115200 11 12 1 1 1 255 190 5","1 115200 11 12 0 242 1 255 190 5",
        "1 115200 11 12 1 242 1 255 190 51","1 115200 11 12 1 242 1 1 242 5",
        "1 115200 11 12 1 242 1 255 190 5 extra","1 115200 11 12 1 242 1 255 190",
        "1 115200 11 12 257 242 1 255 190 5","-1 115200 11 12 1 242 1 255 190 5",
        "1 9999999999999999999999999999999999 11 12 1 242 1 255 190 5"};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        fc_config_t before=config;
        assert(!fc_config_parse(&config,invalid[i])); assert(!memcmp(&before,&config,sizeof(config)));
    }
    fc_link_t link;
    fc_link_init(&link,&config,mac,UINT64_C(0x123456789abcdef0));
    fc_receive(&link,fc_position,sizeof(fc_position),100); assert(!link.telemetry.position_seen);
    fc_receive(&link,other_heartbeat,sizeof(other_heartbeat),100); assert(!link.telemetry.heartbeat_seen);
    fc_receive(&link,gcs_heartbeat,sizeof(gcs_heartbeat),100); assert(!link.telemetry.heartbeat_seen);
    fc_receive(&link,signed_heartbeat,sizeof(signed_heartbeat),100); assert(!link.telemetry.heartbeat_seen);
    assert(link.telemetry.signed_ignored==1);
    for (size_t i=0;i<sizeof(reference_heartbeat);++i) fc_receive(&link,reference_heartbeat+i,1,100);
    assert(fc_heartbeat_fresh(&link.telemetry,100)); assert(link.telemetry.custom_mode==17);
    fc_receive(&link,fc_position,sizeof(fc_position),200);
    assert(link.telemetry.lat==377749000 && link.telemetry.lon==-1224194000);
    assert(link.telemetry.altitude_mm==120500 && link.telemetry.relative_altitude_mm==20500);
    fc_receive(&link,fc_gps,sizeof(fc_gps),300); assert(link.telemetry.fix_type==3 && link.telemetry.satellites==12);
    assert(!fc_heartbeat_fresh(&link.telemetry,5000100));
    fc_receive(&link,fc_position,sizeof(fc_position),6000000); assert(link.telemetry.position_us==200);
    fc_receive(&link,reference_heartbeat_v1,sizeof(reference_heartbeat_v1),6000000); assert(fc_heartbeat_fresh(&link.telemetry,6000000));
    uint8_t corrupt[sizeof(reference_heartbeat)]; memcpy(corrupt,reference_heartbeat,sizeof(corrupt));corrupt[12]^=1;
    fc_receive(&link,corrupt,sizeof(corrupt),7000000);assert(link.telemetry.heartbeat_us==6000000);
    assert(link.telemetry.parse_errors>0);
    fc_receive(&link,reference_heartbeat,sizeof(reference_heartbeat),7000000);assert(link.telemetry.heartbeat_us==7000000);
    for (size_t n=0;n<sizeof(reference_heartbeat);++n) {
        fc_link_t partial;fc_link_init(&partial,&config,mac,1);
        fc_receive(&partial,reference_heartbeat,n,100);assert(!partial.telemetry.heartbeat_seen);
    }
    uint32_t seed=42;uint8_t noise[1000];
    for (unsigned n=0;n<1000;++n) {
        for (unsigned i=0;i<sizeof(noise);++i) {seed=seed*1664525+1013904223;noise[i]=seed>>24;}
        fc_receive(&link,noise,sizeof(noise),8000000);
    }
    uint8_t out[MAVLINK_MAX_PACKET_LEN];
    fc_link_init(&link,&config,mac,UINT64_C(0x123456789abcdef0));
    size_t len=fc_heartbeat(&link,out);assert(out[0]==0xfd);fixture("heartbeat",out,len);
    rid_identity_t identity={.id="SCOUT-01",.hello="UART test"};
    uint8_t adv[RID_ADV_SIZE];assert(rid_identity_advertisement(&identity,false,7,adv));
    rid_observation_t o={.seq=42,.rx_us=1000000,.rssi=-47,.transport=RID_WIFI_BEACON,.channel=6,.pack_size=2};
    memcpy(o.address,mac,6);assert(rid_parse_legacy(adv,sizeof(adv),&o.payload)==1);
    len=fc_observation(&link,&o,1001234,out);fixture("observation",out,len);
    uint32_t counters[11]={1,2,3,4,5,6,UINT32_MAX,8,9,10,11};
    len=fc_health(&link,2,2000000,counters,out);fixture("health",out,len);
    fprintf(stderr,"UART configuration, MAVLink reference frames, CRC recovery, age checks and parser fuzz passed.\n");
}
