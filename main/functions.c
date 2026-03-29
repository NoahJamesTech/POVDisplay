#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_now.h"
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
volatile uint16_t gTargetRpm = 0;
volatile uint16_t gActualRpm = 0;
volatile uint8_t gMotorStatus = 0;
volatile uint8_t gArrowState = POV_ARROW_STEADY;
volatile uint32_t gTelemetryLastMs = 0;

static const char *TAG = "POV";

// MAC address of the motor controller board (POVMotor)
static uint8_t controller_mac[ESP_NOW_ETH_ALEN] = {0x94, 0xA9, 0x90, 0x37, 0x2F, 0x6C};

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
    ESP_ERROR_CHECK(esp_wifi_set_channel(WEB_AP_CHANNEL, WIFI_SECOND_CHAN_NONE));

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

static void espnow_display_recv_cb(const esp_now_recv_info_t *info,
                                   const uint8_t *data, int len)
{
    (void)info;
    if (len != sizeof(pov_packet_v2_t)) return;
    const pov_packet_v2_t *pkt = (const pov_packet_v2_t *)data;
    if (pkt->msg_type != POV_MSG_STATUS) return;

    gTargetRpm = pkt->target_rpm;
    gActualRpm = pkt->actual_rpm;
    gMotorStatus = pkt->motor_status;
    gArrowState = pkt->arrow;
    gTelemetryLastMs = (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void espnow_display_send_cb(const esp_now_send_info_t *info,
                                   esp_now_send_status_t status)
{
    (void)info;
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "ESP-NOW control send failed");
    }
}

void espnowDisplayInit(void)
{
    uint8_t display_sta_mac[6] = {0};
    uint8_t display_ap_mac[6] = {0};
    esp_read_mac(display_sta_mac, ESP_MAC_WIFI_STA);
    esp_read_mac(display_ap_mac, ESP_MAC_WIFI_SOFTAP);
    ESP_LOGI(TAG, ">>> Display STA MAC: {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}",
             display_sta_mac[0], display_sta_mac[1], display_sta_mac[2],
             display_sta_mac[3], display_sta_mac[4], display_sta_mac[5]);
    ESP_LOGI(TAG, ">>> Display AP  MAC: {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}",
             display_ap_mac[0], display_ap_mac[1], display_ap_mac[2],
             display_ap_mac[3], display_ap_mac[4], display_ap_mac[5]);
    ESP_LOGI(TAG, ">>> Controller target MAC: {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}",
             controller_mac[0], controller_mac[1], controller_mac[2],
             controller_mac[3], controller_mac[4], controller_mac[5]);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_display_recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_display_send_cb));

    esp_now_peer_info_t peer = {
        .channel = WEB_AP_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, controller_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

void espnowSendControl(void)
{
    pov_packet_v2_t pkt = {
        .msg_type = POV_MSG_CONTROL,
        .strip_on = gStripOn ? 1 : 0,
        .mode = gMode,
        .brightness = gBrightness,
        .target_rpm = gTargetRpm,
        .actual_rpm = 0,
        .motor_status = 0,
        .arrow = POV_ARROW_STEADY,
    };
    esp_now_send(controller_mac, (const uint8_t *)&pkt, sizeof(pkt));
}
