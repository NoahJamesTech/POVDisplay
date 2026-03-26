#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/spi_master.h"
#include "functions.h"

volatile bool gStripOn = true;
volatile uint8_t gMode = 1;

static const char *TAG = "POV";

// ── Packet layout must match controller ──────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t stripOn;
    uint8_t mode;
} pov_packet_t;

static void espnowRecvCb(const esp_now_recv_info_t *recvInfo, const uint8_t *data, int dataLen)
{
    if (dataLen < (int)sizeof(pov_packet_t)) return;
    const pov_packet_t *pkt = (const pov_packet_t *)data;
    gStripOn = (pkt->stripOn != 0);
    gMode = pkt->mode;
    ESP_LOGI(TAG, "RX strip=%d mode=%d", pkt->stripOn, pkt->mode);
}

void wifiInit(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void espnowInit(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnowRecvCb));
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
