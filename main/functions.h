#ifndef FUNCTIONS_H
#define FUNCTIONS_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"

extern volatile bool gStripOn;
extern volatile uint8_t gMode;
extern volatile uint8_t gBrightness;
extern volatile uint32_t gRotationPeriodUs;
extern volatile uint16_t gActiveImageIndex;
extern volatile uint16_t gTargetRpm;

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
void espnowDisplayInit(void);
void espnowSendControl(void);

#define POV_MSG_CONTROL  0x01
#define POV_MSG_STATUS   0x02

typedef struct __attribute__((packed)) {
	uint8_t  msg_type;
	uint8_t  strip_on;
	uint8_t  mode;
	uint8_t  brightness;
	uint16_t target_rpm;
	uint16_t actual_rpm;
	uint8_t  motor_status;
	uint8_t  reserved;
} pov_packet_v2_t;

#endif // FUNCTIONS_H
