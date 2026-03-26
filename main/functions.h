#ifndef FUNCTIONS_H
#define FUNCTIONS_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"

extern volatile bool gStripOn;
extern volatile uint8_t gMode;

#define DOTSTAR_NUM_LEDS   72
#define TARGET_FPS         7000
#define FRAME_INTERVAL_US  (1000000 / (TARGET_FPS))  // microseconds between frames
#define DOTSTAR_SPI_HOST   SPI2_HOST
#define DOTSTAR_END_BYTES  (4 + ((DOTSTAR_NUM_LEDS + 15) / 16))
#define DOTSTAR_BUF_LEN    (4 + (DOTSTAR_NUM_LEDS * 4) + DOTSTAR_END_BYTES)

extern spi_device_handle_t dotstarDev;

void dotstarSetPixel(uint32_t i, uint8_t brightness031, uint8_t r, uint8_t g, uint8_t b);
void initBuffer(void);
void dotstarShow(void);
void dotstarShowWait(void);

void wifiInit(void);
void espnowInit(void);

#endif // FUNCTIONS_H
