#pragma once

#include "driver/gpio.h"

// Seeed XIAO ESP32-C5 user LED, active low.
// https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/
#define BOARD_LED_GPIO GPIO_NUM_27
#define BOARD_LED_ON_LEVEL 0
#define BOARD_LED_OFF_LEVEL 1
