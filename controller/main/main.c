#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"

static const char *TAG = "POVMotor";

// ── MAC address of the POVDisplay board ───────────────────────────────────────
static uint8_t receiver_mac[ESP_NOW_ETH_ALEN] = {0x94, 0xA9, 0x90, 0x37, 0x2F, 0x6C};

// ── ESC PWM output ────────────────────────────────────────────────────────────
// TODO: set this to the actual GPIO pin connected to the ESC signal wire
#define ESC_PWM_PIN       1

// ── ESC PWM parameters ───────────────────────────────────────────────────────
// 50Hz, 1.0-2.0ms pulse width (standard ESC servo signal)
#define ESC_PWM_FREQ_HZ     50
#define ESC_PWM_RESOLUTION   LEDC_TIMER_14_BIT
#define ESC_MIN_PULSE_US   1000
#define ESC_MAX_PULSE_US   2000
#define INITIAL_SPIN_COUNT 3

#define ESC_TIMER_TOP      ((1U << ESC_PWM_RESOLUTION) - 1U)
#define ESC_PERIOD_US      (1000000U / ESC_PWM_FREQ_HZ)

static const uint32_t ESC_DUTY_MIN = (ESC_TIMER_TOP * ESC_MIN_PULSE_US) / ESC_PERIOD_US;
static const uint32_t ESC_DUTY_MAX = (ESC_TIMER_TOP * ESC_MAX_PULSE_US) / ESC_PERIOD_US;

// ── ESC duty from RPM ─────────────────────────────────────────────────────────
// Maps target RPM (0-3000) linearly to ESC duty (min-max).
static uint32_t rpm_to_esc_duty(uint16_t rpm)
{
    if (rpm == 0) return ESC_DUTY_MIN;
    if (rpm >= 3000) return ESC_DUTY_MAX;
    return ESC_DUTY_MIN +
           (uint32_t)((uint64_t)(ESC_DUTY_MAX - ESC_DUTY_MIN) * rpm / 3000);
}

static void esc_initialSpin(void)
{
    uint32_t duty = ESC_DUTY_MIN +
                    (uint32_t)(((uint64_t)(ESC_DUTY_MAX - ESC_DUTY_MIN) * 25U) / 100U);

    for (int i = 0; i < INITIAL_SPIN_COUNT; i++) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        vTaskDelay(pdMS_TO_TICKS(250));

        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, ESC_DUTY_MIN);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

// ── ESP-NOW packet v2 (shared with display) ───────────────────────────────────
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

// ── Motor state ───────────────────────────────────────────────────────────────
static volatile uint16_t g_target_rpm = 0;  // start at 0 (idle) until display sends a value
static volatile uint16_t g_current_rpm = 0; // last RPM value applied to ESC output

// ── ESP-NOW callbacks ─────────────────────────────────────────────────────────
static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "Send FAIL");
    }
}

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len)
{
    if (len != sizeof(pov_packet_v2_t)) return;
    const pov_packet_v2_t *pkt = (const pov_packet_v2_t *)data;

    if (pkt->msg_type == POV_MSG_CONTROL) {
        uint16_t current_rpm = g_current_rpm;
        if (pkt->target_rpm != g_target_rpm) {
            ESP_LOGI(TAG, "RPM change cmd: current=%u -> requested=%u",
                     current_rpm, pkt->target_rpm);
        }
        g_target_rpm = pkt->target_rpm;
    }
}

// ── Motor output task — updates ESC PWM from target RPM ───────────────────────
static void motor_task(void *arg)
{
    uint16_t last_rpm = 0xFFFF;  // force initial update

    // Hold minimum throttle so ESC can arm.
    g_target_rpm = 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, ESC_DUTY_MIN);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "ESC idle for arming: 7000ms");
    vTaskDelay(pdMS_TO_TICKS(7000));
    ESP_LOGW(TAG, "ESC initialSpin enabled: remove prop before use");
    esc_initialSpin();

    while (1) {
        uint16_t rpm = g_target_rpm;

        if (rpm != last_rpm) {
            uint32_t duty = rpm_to_esc_duty(rpm);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            last_rpm = rpm;
            g_current_rpm = rpm;
            ESP_LOGI(TAG, "ESC duty: %u (RPM %u)", (unsigned)duty, rpm);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ── Peripheral init ───────────────────────────────────────────────────────────
static void esc_pwm_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = ESC_PWM_RESOLUTION,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = ESC_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = ESC_PWM_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = ESC_DUTY_MIN,  // start at idle
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    ESP_LOGI(TAG, "ESC PWM: %dHz on GPIO%d, duty range %u-%u",
             ESC_PWM_FREQ_HZ, ESC_PWM_PIN, ESC_DUTY_MIN, ESC_DUTY_MAX);
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
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

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

    esc_pwm_init();
    wifi_init();

    // Print this board's MAC so you can paste it into the display's controller_mac
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, ">>> Controller MAC: {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, ">>> Configured Display MAC: {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}",
             receiver_mac[0], receiver_mac[1], receiver_mac[2],
             receiver_mac[3], receiver_mac[4], receiver_mac[5]);
    if (memcmp(mac, receiver_mac, ESP_NOW_ETH_ALEN) == 0) {
        ESP_LOGW(TAG, "Configured display MAC matches controller MAC; update receiver_mac to the display board MAC");
    }

    espnow_init();

    xTaskCreate(motor_task, "motor", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Motor controller ready (open-loop, waiting for target RPM via ESP-NOW)");
}
