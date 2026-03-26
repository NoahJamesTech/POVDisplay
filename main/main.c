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
#include "functions.h"

// Change this as needed

#define DOTSTAR_DATA_GPIO  1  // -> DI
#define DOTSTAR_CLK_GPIO   2  // -> CI
#define DOTSTAR_CS_GPIO    13  // CS for oscilloscope timing measurement (directly usable, no connection to strip needed)
#define BLADE_LEDS          (DOTSTAR_NUM_LEDS / 2)   // 36
// 500 RPM = 120ms per full rotation = 120000 us
#define ROTATION_PERIOD_US  120000
// 6 color slots of 60° each across the full rotation
#define NUM_SLOTS           6
#define SLOT_US             (ROTATION_PERIOD_US / NUM_SLOTS)  // 10000 us = 60° each

// Center-pivot: LEDs 0-35 = blade A, LEDs 36-71 = blade B (180° opposite)

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
