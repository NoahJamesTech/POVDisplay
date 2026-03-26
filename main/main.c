//#### BLADE CODE #####//

#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"
#include "pov_image.h"
#include "pov_image2.h"
#include "pov_image3.h"


static const char *TAG = "POV";

// ── Shared state (written by ESP-NOW callback on core 0, read by led_task on core 1) ──
static volatile bool gStripOn = true;
static volatile uint8_t gMode = 1;

// ── Packet layout must match controller ──────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t stripOn;
    uint8_t mode;
} pov_packet_t;


// Change this as needed

#define DOTSTAR_DATA_GPIO  1  // -> DI
#define DOTSTAR_CLK_GPIO   2  // -> CI
#define DOTSTAR_CS_GPIO    13  // CS for oscilloscope timing measurement (directly usable, no connection to strip needed)
#define DOTSTAR_NUM_LEDS   72
#define TARGET_FPS         7000
#define FRAME_INTERVAL_US  (1000000 / (TARGET_FPS))  // microseconds between frames
#define BLADE_LEDS          (DOTSTAR_NUM_LEDS / 2)   // 36

// 500 RPM = 120ms per full rotation = 120000 us
#define ROTATION_PERIOD_US  120000
// 6 color slots of 60° each across the full rotation
#define NUM_SLOTS           6
#define SLOT_US             (ROTATION_PERIOD_US / NUM_SLOTS)  // 10000 us = 60° each

// Center-pivot: LEDs 0-35 = blade A, LEDs 36-71 = blade B (180° opposite)


#define DOTSTAR_SPI_HOST   SPI2_HOST
#define DOTSTAR_END_BYTES  (4 + ((DOTSTAR_NUM_LEDS + 15) / 16))
#define DOTSTAR_BUF_LEN    (4 + (DOTSTAR_NUM_LEDS * 4) + DOTSTAR_END_BYTES)

static spi_device_handle_t dotstarDev;
static uint8_t *dotstarBuf  = NULL;
static uint8_t *dotstarLeds = NULL;  // points to first LED frame (after 4-byte start)
static spi_transaction_t dotstarTrans;
static int64_t lastFrameTime = 0;

static inline __attribute__((always_inline))
void dotstarSetPixel(uint32_t i, uint8_t brightness031, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p = dotstarLeds + (i * 4);
    p[0] = 0xE0 | (brightness031 & 0x1F); // 0b111xxxxx
    p[1] = b;
    p[2] = g;
    p[3] = r;
}

static void initBuffer(void)
{
    dotstarBuf = (uint8_t *)heap_caps_malloc(DOTSTAR_BUF_LEN, MALLOC_CAP_DMA);
    assert(dotstarBuf);
    memset(dotstarBuf, 0x00, 4);
    dotstarLeds = dotstarBuf + 4;
    memset(dotstarBuf + 4 + (DOTSTAR_NUM_LEDS * 4), 0xFF, DOTSTAR_END_BYTES);
    memset(&dotstarTrans, 0, sizeof(dotstarTrans));
    dotstarTrans.length    = DOTSTAR_BUF_LEN * 8;
    dotstarTrans.tx_buffer = dotstarBuf;
}


static inline __attribute__((always_inline))


// No wait - fire SPI as fast as possible (use for max rate testing)
void dotstarShow(void)
{
    ESP_ERROR_CHECK(spi_device_polling_transmit(dotstarDev, &dotstarTrans));
}


// With frame rate limiting
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

static void espnowRecvCb(const esp_now_recv_info_t *recvInfo, const uint8_t *data, int dataLen)
{
    if (dataLen < (int)sizeof(pov_packet_t)) return;
    const pov_packet_t *pkt = (const pov_packet_t *)data;
    gStripOn = (pkt->stripOn != 0);
    gMode = pkt->mode;
    ESP_LOGI(TAG, "RX strip=%d mode=%d", pkt->stripOn, pkt->mode);
}

static void wifiInit(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void espnowInit(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnowRecvCb));
}

static const uint8_t SLOT_COLORS[NUM_SLOTS][3] = {
    {255,   0,   0},   // 0: Red
    {255, 128,   0},   // 1: Orange
    {255, 255,   0},   // 2: Yellow
    {  0, 255,   0},   // 3: Green
    {  0,   0, 255},   // 4: Blue
    {148,   0, 211},   // 5: Violet
};

static void ledTask(void *arg)
{
    // Remove this task from watchdog monitoring since we use tight spin-wait loops
    esp_task_wdt_delete(xTaskGetCurrentTaskHandle());

    int64_t startTime = esp_timer_get_time();

    while (1) {
        if (!gStripOn) {
            // All off
            for (uint32_t i = 0; i < DOTSTAR_NUM_LEDS; i++) {
                dotstarSetPixel(i, 0, 0, 0, 0);
            }
            dotstarShow();
            continue;
        }

        uint8_t mode = gMode;

        if (mode == 1) {
            int64_t now = esp_timer_get_time();
            int64_t elapsed = now - startTime;

            uint32_t positionInRotation = (uint32_t)(elapsed % ROTATION_PERIOD_US);
            uint32_t slotA = positionInRotation / SLOT_US;
            if (slotA >= NUM_SLOTS) slotA = NUM_SLOTS - 1;

            // Blade B is always 180° (3 slots) behind blade A
            uint32_t slotB = (slotA + 3) % NUM_SLOTS;

            // Blade A: LEDs 0-35
            for (uint32_t i = 0; i < BLADE_LEDS; i++) {
                dotstarSetPixel(i, 15,
                    SLOT_COLORS[slotA][0],
                    SLOT_COLORS[slotA][1],
                    SLOT_COLORS[slotA][2]);
            }
            // Blade B: LEDs 36-71
            for (uint32_t i = BLADE_LEDS; i < DOTSTAR_NUM_LEDS; i++) {
                dotstarSetPixel(i, 15,
                    SLOT_COLORS[slotB][0],
                    SLOT_COLORS[slotB][1],
                    SLOT_COLORS[slotB][2]);
            }
        } 
        else if (mode == 2 || mode == 3 || mode == 4) 
        {
            int64_t now = esp_timer_get_time();
            int64_t elapsed = now - startTime;

            uint32_t positionInRotation = (uint32_t)(elapsed % ROTATION_PERIOD_US);

            uint32_t colA = (positionInRotation * 720) / ROTATION_PERIOD_US;
            if (colA >= 720) colA = 719;
            uint32_t colB = (colA + 360) % 720;

            for (uint32_t i = 0; i < BLADE_LEDS; i++) 
            {
                if (mode == 2) {
                    dotstarSetPixel(i, 15,
                        pov_image[colA][BLADE_LEDS - 1 - i][0],
                        pov_image[colA][BLADE_LEDS - 1 - i][1],
                        pov_image[colA][BLADE_LEDS - 1 - i][2]);
                    dotstarSetPixel(i + BLADE_LEDS, 15,
                        pov_image[colB][i][0],
                        pov_image[colB][i][1],
                        pov_image[colB][i][2]);
                } else if (mode == 3) {
                    dotstarSetPixel(i, 15,
                        pov_image2[colA][BLADE_LEDS - 1 - i][0],
                        pov_image2[colA][BLADE_LEDS - 1 - i][1],
                        pov_image2[colA][BLADE_LEDS - 1 - i][2]);
                    dotstarSetPixel(i + BLADE_LEDS, 15,
                        pov_image2[colB][i][0],
                        pov_image2[colB][i][1],
                        pov_image2[colB][i][2]);
                } else if (mode == 4) {
                    dotstarSetPixel(i, 15,
                        pov_image3[colA][BLADE_LEDS - 1 - i][0],
                        pov_image3[colA][BLADE_LEDS - 1 - i][1],
                        pov_image3[colA][BLADE_LEDS - 1 - i][2]);
                    dotstarSetPixel(i + BLADE_LEDS, 15,
                        pov_image3[colB][i][0],
                        pov_image3[colB][i][1],
                        pov_image3[colB][i][2]);
                } else {

                }
            }
        }
        else if (mode == 5)
        {
            for (uint32_t i = 0; i < DOTSTAR_NUM_LEDS; i++) 
            {
                if (i == 71 || i == 0) {
                    dotstarSetPixel(i, 15, 0, 255, 0); // Blue
                } else {
                    dotstarSetPixel(i, 0, 0, 0, 0); // Off
                }
            }
        }
        else 
        {
            // Unknown mode — turn LEDs off
            for (uint32_t i = 0; i < DOTSTAR_NUM_LEDS; i++) {
                dotstarSetPixel(i, 0, 0, 0, 0);
            }
        }

        dotstarShow();
    }
}




void app_main(void)
{
    // Remove IDLE task on core 1 from watchdog - we'll starve it intentionally for precise LED timing
    esp_task_wdt_delete(xTaskGetIdleTaskHandleForCore(1));

    spi_bus_config_t buscfg = {
        .mosi_io_num = DOTSTAR_DATA_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = DOTSTAR_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DOTSTAR_BUF_LEN, 
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 40 * 1000 * 1000, 
        .mode = 0,
        .spics_io_num = DOTSTAR_CS_GPIO,  // CS toggles each frame - use for scope measurement
        .queue_size = 1,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(DOTSTAR_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(DOTSTAR_SPI_HOST, &devcfg, &dotstarDev));
    initBuffer();

    // Core 0: Wi-Fi + ESP-NOW receiver
    ESP_ERROR_CHECK(nvs_flash_init());
    wifiInit();
    espnowInit();

    // Run the LED loop on core 1 so core 0 is free for Wi-Fi/ESP-NOW
    xTaskCreatePinnedToCore(ledTask, "ledTask", 4096, NULL, 24, NULL, 1);
    
}
