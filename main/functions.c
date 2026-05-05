#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "functions.h"

volatile bool gStripOn = true;
volatile int gMode = 2;
volatile int gBrightness = 15;
volatile int gRotationPeriodUs = 120000;
volatile int gActiveImageIndex = 0;

spi_device_handle_t dotstarDev;
static unsigned char *dotstarBuf  = NULL;

static unsigned char *dotstarLeds = NULL;  // points to first LED frame (after 4-byte start)
static spi_transaction_t dotstarTrans;
static int64_t lastFrameTime = 0;

void dotstarSetPixel(int i, int brightness031, int r, int g, int b)
{
    if (i < 0 || i >= DOTSTAR_NUM_LEDS) {
        return;
    }
    if (brightness031 < 0) brightness031 = 0;
    if (brightness031 > 31) brightness031 = 31;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;

    unsigned char *p = dotstarLeds + (i * 4);
    p[0] = 0xE0 | (brightness031 & 0x1F); // 0b111xxxxx
    p[1] = (unsigned char)b;
    p[2] = (unsigned char)g;
    p[3] = (unsigned char)r;
}

void initBuffer(void)
{
    dotstarBuf = (unsigned char *)heap_caps_malloc(DOTSTAR_BUF_LEN, MALLOC_CAP_DMA);
    assert(dotstarBuf);
    memset(dotstarBuf, 0x00, 4);
    dotstarLeds = dotstarBuf + 4;
    memset(dotstarBuf + 4 + (DOTSTAR_NUM_LEDS * 4), 0xFF, DOTSTAR_END_BYTES);
    memset(&dotstarTrans, 0, sizeof(dotstarTrans));
    dotstarTrans.length    = DOTSTAR_BUF_LEN * 8;
    dotstarTrans.tx_buffer = dotstarBuf;
}

void dotstarShow(void)
{
    ESP_ERROR_CHECK(spi_device_polling_transmit(dotstarDev, &dotstarTrans));
}

void dotstarShowWait(void)
{
    int64_t now = esp_timer_get_time();
    int64_t waitUs = FRAME_INTERVAL_US - (now - lastFrameTime);
    
    if (waitUs > 0) {
        int64_t target = now + waitUs;
        while (esp_timer_get_time() < target) {}
    }
    
    lastFrameTime = esp_timer_get_time();
    ESP_ERROR_CHECK(spi_device_polling_transmit(dotstarDev, &dotstarTrans));
}
