#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"

static const char *TAG = "POVController";

// ── MAC address of the POVDisplay receiver ────────────────────────────────────
static uint8_t receiver_mac[ESP_NOW_ETH_ALEN] = {0x94, 0xA9, 0x90, 0x37, 0x2F, 0xFC};

// ── Pin definitions ───────────────────────────────────────────────────────────
#define BTN_ONOFF_PIN     9   // toggle strip on/off
#define BTN_MODE_PIN      13  // cycle through modes

#define LED_CONNECTED_PIN 7   // lit = receiver is respondin
#define LED_ONOFF_PIN     8   // mirrors strip on/off state

// Mode indicator LEDs — index 0 = mode 1, index 4 = mode 5
static const int MODE_LED_PINS[5] = {1, 2, 3, 4, 5};

#define NUM_MODES         5
#define DEBOUNCE_MS       50

// ── Packet sent to the display ────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t strip_on;  // 1 = on, 0 = off
    uint8_t mode;      // 1–5
} pov_packet_t;

// ── Shared state ──────────────────────────────────────────────────────────────
static volatile bool     g_strip_on    = true;
static volatile uint8_t  g_mode        = 1;
static volatile bool     g_send_needed = false;
static SemaphoreHandle_t g_state_mutex;

// ── Helpers ───────────────────────────────────────────────────────────────────
static void update_leds(bool strip_on, uint8_t mode)
{
    gpio_set_level(LED_ONOFF_PIN, strip_on ? 1 : 0);

    for (int i = 0; i < NUM_MODES; i++) {
        gpio_set_level(MODE_LED_PINS[i], (i + 1 == mode) ? 1 : 0);
    }
}

// ── ESP-NOW callbacks ─────────────────────────────────────────────────────────
static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    gpio_set_level(LED_CONNECTED_PIN, (status == ESP_NOW_SEND_SUCCESS) ? 1 : 0);
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "Send FAIL");
    }
}

// ── Button task ───────────────────────────────────────────────────────────────
static void button_task(void *arg)
{
    bool     last_onoff = true;   // assume buttons released at start
    bool     last_mode  = true;
    uint32_t onoff_time = 0;
    uint32_t mode_time  = 0;

    while (1) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // Active-low buttons (internal pull-up)
        bool onoff_pressed = (gpio_get_level(BTN_ONOFF_PIN) == 0);
        bool mode_pressed  = (gpio_get_level(BTN_MODE_PIN)  == 0);

        // On/off button — falling-edge detect with debounce
        if (onoff_pressed && !last_onoff && (now - onoff_time) >= DEBOUNCE_MS) {
            onoff_time = now;
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_strip_on    = !g_strip_on;
            g_send_needed = true;
            bool    s = g_strip_on;
            uint8_t m = g_mode;
            xSemaphoreGive(g_state_mutex);
            update_leds(s, m);
            ESP_LOGI(TAG, "Strip -> %s", s ? "ON" : "OFF");
        }

        // Mode button — falling-edge detect with debounce
        if (mode_pressed && !last_mode && (now - mode_time) >= DEBOUNCE_MS) {
            mode_time = now;
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_mode = (g_mode % NUM_MODES) + 1;  // 1→2→3→4→5→1
            g_send_needed = true;
            bool    s = g_strip_on;
            uint8_t m = g_mode;
            xSemaphoreGive(g_state_mutex);
            update_leds(s, m);
            ESP_LOGI(TAG, "Mode -> %d", m);
        }

        last_onoff = onoff_pressed;
        last_mode  = mode_pressed;

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── Send task — fires only when state changes ─────────────────────────────────
static void send_task(void *arg)
{
    while (1) {
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        bool needed = g_send_needed;
        pov_packet_t pkt = {
            .strip_on = g_strip_on ? 1 : 0,
            .mode     = g_mode,
        };
        if (needed) g_send_needed = false;
        xSemaphoreGive(g_state_mutex);

        if (needed) {
            esp_err_t err = esp_now_send(receiver_mac, (uint8_t *)&pkt, sizeof(pkt));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_now_send: %s", esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── Peripheral init ───────────────────────────────────────────────────────────
static void gpio_init(void)
{
    // Buttons — input with pull-up (active-low)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_ONOFF_PIN) | (1ULL << BTN_MODE_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));

    // Status LEDs — output
    uint64_t led_mask = (1ULL << LED_CONNECTED_PIN) | (1ULL << LED_ONOFF_PIN);
    for (int i = 0; i < NUM_MODES; i++) {
        led_mask |= (1ULL << MODE_LED_PINS[i]);
    }
    gpio_config_t led_cfg = {
        .pin_bit_mask = led_mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));

    // Initial LED state
    update_leds(g_strip_on, g_mode);
    gpio_set_level(LED_CONNECTED_PIN, 0);
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void espnow_init(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));

    esp_now_peer_info_t peer = {
        .channel = 0,
        .ifidx   = ESP_IF_WIFI_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, receiver_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

// ── Entry point ───────────────────────────────────────────────────────────────
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    g_state_mutex = xSemaphoreCreateMutex();

    gpio_init();
    wifi_init();
    espnow_init();

    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
    xTaskCreate(send_task,   "send_task",   4096, NULL, 4, NULL);
}
