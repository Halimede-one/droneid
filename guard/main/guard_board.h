#pragma once
#include "sdkconfig.h"
// Same XIAO pad labels, different silicon GPIO numbers. D6/D7 (ROM UART),
// boot/strapping pins, and external JTAG pins remain unconnected.
#if CONFIG_IDF_TARGET_ESP32C5
#define GUARD_BOARD_ID "xiao_esp32c5"
#define SENSOR_TX 8
#define SENSOR_RX 9
#define FC_TX 23
#define FC_RX 24
#elif CONFIG_IDF_TARGET_ESP32C6
#define GUARD_BOARD_ID "xiao_esp32c6"
#define SENSOR_TX 19
#define SENSOR_RX 20
#define FC_TX 22
#define FC_RX 23
#elif CONFIG_IDF_TARGET_ESP32S3
#define GUARD_BOARD_ID "xiao_esp32s3"
#define SENSOR_TX 7
#define SENSOR_RX 8
#define FC_TX 5
#define FC_RX 6
#else
#error "No reviewed XIAO guard pin mapping for this target"
#endif
