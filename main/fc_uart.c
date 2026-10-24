#include "fc_uart.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/task.h"

#define FC_UART UART_NUM_1
#define FC_QUEUE_SIZE 32
#define FC_MAX_QUEUE_AGE_US UINT64_C(2000000)

static fc_link_t link_state;
static fc_telemetry_t telemetry;
static portMUX_TYPE telemetry_lock=portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t observations, events;
static bench_radio_stats_t *radio_stats;
static const _Atomic uint32_t *usb_drops;
static _Atomic bool ready;
static _Atomic uint32_t queue_drops, expired, tx_frames, uart_errors;

void fc_uart_submit(const rid_observation_t *o)
{
    if (observations && xQueueSend(observations,o,0)!=pdTRUE) atomic_fetch_add(&queue_drops,1);
}

static void uart_worker(void *unused)
{
    (void)unused;
    uint8_t input[256], output[MAVLINK_MAX_PACKET_LEN];
    size_t pending=0, offset=0;
    uint64_t pending_since=0, next_heartbeat=0, next_health=0, next_observation=0;
    uint32_t health_sequence=0;
    for (;;) {
        uint64_t now=(uint64_t)esp_timer_get_time();
        uart_event_t event;
        for (unsigned i=0; i<16 && xQueueReceive(events,&event,0)==pdTRUE; ++i) {
            if (event.type==UART_FIFO_OVF || event.type==UART_BUFFER_FULL ||
                event.type==UART_PARITY_ERR || event.type==UART_FRAME_ERR || event.type==UART_BREAK) {
                atomic_fetch_add(&uart_errors,1);
                uart_flush_input(FC_UART);
                memset(&link_state.rx_status,0,sizeof(link_state.rx_status));
            }
        }
        int count=uart_read_bytes(FC_UART,input,sizeof(input),0);
        if (count>0) fc_receive(&link_state,input,(size_t)count,now);
        else if (count<0) atomic_fetch_add(&uart_errors,1);
        portENTER_CRITICAL(&telemetry_lock);
        telemetry=link_state.telemetry;
        portEXIT_CRITICAL(&telemetry_lock);

        if (!pending) {
            if (now>=next_heartbeat) {
                pending=fc_heartbeat(&link_state,output); next_heartbeat=now+1000000;
            } else if (now>=next_health) {
                fc_telemetry_t *t=&link_state.telemetry;
                uint64_t age=t->heartbeat_seen ? (now-t->heartbeat_us)/1000 : UINT32_MAX;
                uint32_t health[11]={atomic_load(&queue_drops),atomic_load(&expired),
                    atomic_load(&tx_frames),t->rx_frames,t->parse_errors,atomic_load(&uart_errors),
                    age>UINT32_MAX?UINT32_MAX:(uint32_t)age,
                    atomic_load(&radio_stats->radio_errors),atomic_load(&radio_stats->queue_drops),
                    atomic_load(usb_drops),atomic_load(&radio_stats->observations)};
                pending=fc_health(&link_state,++health_sequence,now,health,output);
                next_health=now+2000000;
            } else if (now>=next_observation) {
                rid_observation_t observation;
                // Bound processing even when a noisy RF environment fills the queue.
                for (unsigned i=0; i<FC_QUEUE_SIZE && xQueueReceive(observations,&observation,0)==pdTRUE; ++i) {
                    if (now<observation.rx_us || now-observation.rx_us>FC_MAX_QUEUE_AGE_US) {
                        atomic_fetch_add(&expired,1); continue;
                    }
                    pending=fc_observation(&link_state,&observation,now,output);
                    next_observation=now+1000000/link_state.config.max_rate;
                    break;
                }
            }
            offset=0; pending_since=now;
        }
        if (pending) {
            // Nonblocking FIFO writes; no USB/radio task waits for UART progress.
            int sent=uart_tx_chars(FC_UART,(const char *)output+offset,pending-offset);
            if (sent>0) offset+=(size_t)sent;
            if (offset==pending) {pending=0; atomic_fetch_add(&tx_frames,1);}
            else if (sent<0 || now-pending_since>250000) {pending=0; atomic_fetch_add(&uart_errors,1);}
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void fc_uart_start(const fc_config_t *config, const uint8_t mac[6], uint64_t session,
                   bench_radio_stats_t *radio, const _Atomic uint32_t *drops)
{
    fc_link_init(&link_state,config,mac,session);
    radio_stats=radio; usb_drops=drops;
    if (!config->enabled) return;
    uart_config_t setup={.baud_rate=(int)config->baud,.data_bits=UART_DATA_8_BITS,
        .parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    bool installed=false;
    if (uart_param_config(FC_UART,&setup)!=ESP_OK) goto fail;
    if (uart_set_pin(FC_UART,config->tx_pin,config->rx_pin,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE)!=ESP_OK) goto fail;
    gpio_pullup_en(config->rx_pin);
    if (uart_driver_install(FC_UART,2048,0,16,&events,0)!=ESP_OK) goto fail;
    installed=true;
    observations=xQueueCreate(FC_QUEUE_SIZE,sizeof(rid_observation_t));
    if (!observations) goto fail;
    if (xTaskCreate(uart_worker,"fc_uart",6144,NULL,4,NULL)!=pdPASS) goto fail;
    atomic_store(&ready,true);
    return;
fail:
    if (observations) {vQueueDelete(observations); observations=NULL;}
    if (installed) uart_driver_delete(FC_UART);
    atomic_fetch_add(&uart_errors,1);
}

static int64_t age_ms(bool seen, uint64_t received, uint64_t now)
{
    return seen && now>=received ? (int64_t)((now-received)/1000) : -1;
}

size_t fc_uart_json(char *out, size_t capacity, uint64_t now)
{
    fc_telemetry_t t;
    portENTER_CRITICAL(&telemetry_lock); t=telemetry; portEXIT_CRITICAL(&telemetry_lock);
    const fc_config_t *c=&link_state.config;
    bool connected=atomic_load(&ready) && fc_heartbeat_fresh(&t,now);
    int64_t position_age=age_ms(t.position_seen,t.position_us,now);
    int64_t gps_age=age_ms(t.gps_seen,t.gps_us,now);
    char position[220]="null", gps[100]="null";
    if (connected && position_age>=0 && position_age<5000)
        snprintf(position,sizeof(position),"{\"lat_e7\":%" PRId32 ",\"lon_e7\":%" PRId32
            ",\"altitude_msl_mm\":%" PRId32 ",\"relative_altitude_mm\":%" PRId32 "}",
            t.lat,t.lon,t.altitude_mm,t.relative_altitude_mm);
    if (connected && gps_age>=0 && gps_age<5000)
        snprintf(gps,sizeof(gps),"{\"fix_type\":%u,\"satellites\":%u}",t.fix_type,t.satellites);
    int n=snprintf(out,capacity,
        "{\"configuration_version\":1,\"enabled\":%s,\"ready\":%s,\"baud\":%" PRIu32
        ",\"tx_pin\":%u,\"rx_pin\":%u,\"system_id\":%u,\"component_id\":%u,\"fc_component_id\":%u,"
        "\"target_system\":%u,\"target_component\":%u,\"max_rate\":%u,\"connected\":%s,"
        "\"heartbeat_age_ms\":%" PRId64 ",\"position_age_ms\":%" PRId64 ",\"gps_age_ms\":%" PRId64 ","
        "\"position\":%s,\"gps\":%s,\"tx_frames\":%" PRIu32 ",\"rx_frames\":%" PRIu32
        ",\"queue_drops\":%" PRIu32 ",\"expired\":%" PRIu32 ",\"parse_errors\":%" PRIu32
        ",\"signed_ignored\":%" PRIu32 ",\"errors\":%" PRIu32 "}",
        c->enabled?"true":"false",atomic_load(&ready)?"true":"false",c->baud,c->tx_pin,c->rx_pin,
        c->system_id,c->component_id,c->fc_component_id,c->target_system,c->target_component,c->max_rate,
        connected?"true":"false",age_ms(t.heartbeat_seen,t.heartbeat_us,now),position_age,gps_age,position,gps,
        atomic_load(&tx_frames),t.rx_frames,atomic_load(&queue_drops),atomic_load(&expired),t.parse_errors,
        t.signed_ignored,atomic_load(&uart_errors));
    return n>0 && (size_t)n<capacity ? (size_t)n : 0;
}
