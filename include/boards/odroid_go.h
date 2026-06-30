#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

#define SOLAR_OS_BOARD_ID "odroid_go"
#define SOLAR_OS_BOARD_NAME "Hardkernel ODROID-GO"
#define SOLAR_OS_BOARD_VENDOR "Hardkernel"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-WROVER"

#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_1
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_3
