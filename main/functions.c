#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "driver/spi_master.h"
#include "functions.h"

volatile bool gStripOn = true;
volatile uint8_t gMode = 2;
volatile uint8_t gBrightness = 15;
volatile uint32_t gRotationPeriodUs = 120000;
volatile uint16_t gActiveImageIndex = 0;

static const char *TAG = "POV";

#define WEB_AP_SSID         "POV Display"
#define WEB_AP_PASS         ""
#define WEB_AP_MAX_CONN     8
#define WEB_AP_CHANNEL      1
#define WEB_AP_IP_OCTET_1   10
#define WEB_AP_IP_OCTET_2   10
#define WEB_AP_IP_OCTET_3   10
#define WEB_AP_IP_OCTET_4   10

void wifiInit(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = WEB_AP_CHANNEL,
            .max_connection = WEB_AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
            .ssid_hidden = 0,
            .beacon_interval = 100,
        },
    };

    strncpy((char *)ap_cfg.ap.ssid, WEB_AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(WEB_AP_SSID);

    if (strlen(WEB_AP_PASS) >= 8) {
        strncpy((char *)ap_cfg.ap.password, WEB_AP_PASS, sizeof(ap_cfg.ap.password));
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    esp_netif_ip_info_t ap_ip_info = {0};
    IP4_ADDR(&ap_ip_info.ip, WEB_AP_IP_OCTET_1, WEB_AP_IP_OCTET_2, WEB_AP_IP_OCTET_3, WEB_AP_IP_OCTET_4);
    IP4_ADDR(&ap_ip_info.gw, WEB_AP_IP_OCTET_1, WEB_AP_IP_OCTET_2, WEB_AP_IP_OCTET_3, WEB_AP_IP_OCTET_4);
    IP4_ADDR(&ap_ip_info.netmask, 255, 255, 255, 192);

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ap_ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Web AP ready: SSID=%s channel=%d ip=10.10.10.10/26", WEB_AP_SSID, WEB_AP_CHANNEL);
}

spi_device_handle_t dotstarDev;
static uint8_t *dotstarBuf  = NULL;

static uint8_t *dotstarLeds = NULL;  // points to first LED frame (after 4-byte start)
static spi_transaction_t dotstarTrans;
static int64_t lastFrameTime = 0;

void dotstarSetPixel(uint32_t i, uint8_t brightness031, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p = dotstarLeds + (i * 4);
    p[0] = 0xE0 | (brightness031 & 0x1F); // 0b111xxxxx
    p[1] = b;
    p[2] = g;
    p[3] = r;
}

void initBuffer(void)
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
