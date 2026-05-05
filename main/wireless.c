#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_partition.h"
#include "lwip/ip4_addr.h"
#include "pov_config.h"
#include "pov_gallery.h"
#include "functions.h"

static const char *TAG = "POVWireless";

#define ROTATION_DELAY_PPM_DEFAULT 500
#define POV_IMAGE_BYTES ((size_t)POV_GLOBAL_COLS * (size_t)POV_GLOBAL_LEDS * (size_t)POV_PIXEL_BYTES)

volatile int gTargetRpm = 0;
volatile int gActualRpm = 0;
volatile int gMotorStatus = 0;
volatile int gArrowState = POV_ARROW_STEADY;
volatile int gTelemetryLastMs = 0;

volatile int gRotationDelayPpm = ROTATION_DELAY_PPM_DEFAULT;
volatile bool gAngleLockEnabled = true;
volatile int gRuntimeImageActive = -1;
volatile size_t gRuntimeImageBytes = 0;
unsigned char *gRuntimeImageBuffers[2] = {NULL, NULL};

static volatile int gStripBrightnessRestore = 15;

static unsigned char controller_mac[ESP_NOW_ETH_ALEN] = {0x94, 0xA9, 0x90, 0x37, 0x2F, 0x6C};

#define WEB_AP_SSID         "POV Display"
#define WEB_AP_PASS         ""
#define WEB_AP_MAX_CONN     8
#define WEB_AP_CHANNEL      1
#define WEB_AP_IP_OCTET_1   10
#define WEB_AP_IP_OCTET_2   10
#define WEB_AP_IP_OCTET_3   10
#define WEB_AP_IP_OCTET_4   10

static const char *WORKBENCH_FALLBACK_HTML =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>POV Workbench</title></head><body style='font-family:sans-serif;padding:16px'>"
    "<h2>POV Workbench file not found</h2>"
    "<p>Flash <code>utilities/workbench.html</code> to SPIFFS as <code>/spiffs/workbench.html</code>.</p>"
    "</body></html>";

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
    gTelemetryLastMs = (int)(esp_timer_get_time() / 1000ULL);
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
    unsigned char display_sta_mac[6] = {0};
    unsigned char display_ap_mac[6] = {0};
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

static void init_runtime_image_buffers(void)
{
    if (gRuntimeImageBuffers[0] && gRuntimeImageBuffers[1]) {
        return;
    }

    if (!gRuntimeImageBuffers[0]) {
        gRuntimeImageBuffers[0] = (unsigned char *)heap_caps_malloc(POV_IMAGE_BYTES, MALLOC_CAP_8BIT);
    }
    if (!gRuntimeImageBuffers[1]) {
        gRuntimeImageBuffers[1] = (unsigned char *)heap_caps_malloc(POV_IMAGE_BYTES, MALLOC_CAP_8BIT);
    }

    bool has0 = (gRuntimeImageBuffers[0] != NULL);
    bool has1 = (gRuntimeImageBuffers[1] != NULL);

    if (!has0 && !has1) {
        ESP_LOGE(TAG, "Failed to allocate runtime image buffers (%u bytes each)", (unsigned)POV_IMAGE_BYTES);
        return;
    }

    if (has0 && has1) {
        return;
    }

    ESP_LOGW(
        TAG,
        "Only one runtime image buffer allocated (%u bytes). Uploads will run in single-buffer mode.",
        (unsigned)POV_IMAGE_BYTES);

    if (!has0 && has1) {
        gRuntimeImageActive = 1;
    }
}

static esp_err_t serve_file_or_fallback(httpd_req_t *req, const char *path)
{
    FILE *file = fopen(path, "rb");
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    if (!file) {
        httpd_resp_send(req, WORKBENCH_FALLBACK_HTML, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char chunk[1024];
    size_t read_len;
    while ((read_len = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        esp_err_t err = httpd_resp_send_chunk(req, chunk, read_len);
        if (err != ESP_OK) {
            fclose(file);
            return err;
        }
    }
    fclose(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return serve_file_or_fallback(req, "/spiffs/workbench.html");
}

static esp_err_t health_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t android_generate_204_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t apple_hotspot_detect_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
}

static esp_err_t microsoft_ncsi_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "Microsoft NCSI");
}

static esp_err_t microsoft_connecttest_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "Microsoft Connect Test");
}

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static esp_err_t options_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t not_found_err_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    ESP_LOGW(TAG, "404 for URI='%s' method=%d", req->uri ? req->uri : "(null)", (int)req->method);
    set_cors_headers(req);
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Nothing matches the given URI");
}

static esp_err_t memory_get_handler(httpd_req_t *req)
{
    size_t spiffs_total = 0;
    size_t spiffs_used = 0;
    esp_err_t spiffs_err = esp_spiffs_info(NULL, &spiffs_total, &spiffs_used);

    if (spiffs_err != ESP_OK) {
        const esp_partition_t *storage_part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
            "storage");
        if (storage_part != NULL) {
            spiffs_total = storage_part->size;
        }
        spiffs_used = 0;
    }

    size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t heap_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t heap_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    size_t spiffs_free = (spiffs_total > spiffs_used) ? (spiffs_total - spiffs_used) : 0;

    char json[320];
    int n = snprintf(
        json,
        sizeof(json),
        "{\"ok\":true,\"flash\":{\"total\":%u,\"used\":%u,\"free\":%u,\"mounted\":%s},\"heap\":{\"total\":%u,\"free\":%u,\"min_free\":%u,\"largest_free_block\":%u}}",
        (unsigned)spiffs_total,
        (unsigned)spiffs_used,
        (unsigned)spiffs_free,
        (spiffs_err == ESP_OK) ? "true" : "false",
        (unsigned)heap_total,
        (unsigned)heap_free,
        (unsigned)heap_min_free,
        (unsigned)heap_largest_block);

    if (n < 0 || n >= (int)sizeof(json)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static bool json_find_key_value(const char *json, const char *key, const char **out)
{
    char pattern[48];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pattern)) {
        return false;
    }

    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += n;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return false;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    *out = p;
    return true;
}

static bool json_read_int(const char *json, const char *key, int *out)
{
    const char *p = NULL;
    if (!json_find_key_value(json, key, &p)) return false;
    char *end = NULL;
    long value = strtol(p, &end, 10);
    if (end == p) return false;
    *out = (int)value;
    return true;
}

static bool json_read_bool(const char *json, const char *key, bool *out)
{
    const char *p = NULL;
    if (!json_find_key_value(json, key, &p)) return false;
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    if (*p == '1') {
        *out = true;
        return true;
    }
    if (*p == '0') {
        *out = false;
        return true;
    }
    return false;
}

static esp_err_t state_get_handler(httpd_req_t *req)
{
    int brightness = gBrightness;
    int rotation_period_us = gRotationPeriodUs;
    int rotation_delay_ppm = gRotationDelayPpm;
    int image_index = gActiveImageIndex;
    int target_rpm = gTargetRpm;
    int actual_rpm = gActualRpm;
    int motor_status = gMotorStatus;
    int arrow = gArrowState;
    int now_ms = (int)(esp_timer_get_time() / 1000ULL);
    int telemetry_last_ms = gTelemetryLastMs;
    int telemetry_age_ms = (telemetry_last_ms <= now_ms) ? (now_ms - telemetry_last_ms) : 0;
    bool telemetry_online = (telemetry_last_ms != 0) && (telemetry_age_ms <= 1000);

    char json[640];
    int n = snprintf(
        json,
        sizeof(json),
        "{\"ok\":true,\"control\":{\"strip_on\":%s,\"brightness\":%d,\"rotation_period_us\":%d,\"rotation_delay_ppm\":%d,\"angle_lock\":%s,\"image_index\":%d,\"target_rpm\":%d},\"telemetry\":{\"actual_rpm\":%d,\"target_rpm\":%d,\"motor_status\":%d,\"arrow\":%d,\"online\":%s,\"age_ms\":%d}}",
        gStripOn ? "true" : "false",
        brightness,
        rotation_period_us,
        rotation_delay_ppm,
        gAngleLockEnabled ? "true" : "false",
        image_index,
        target_rpm,
        actual_rpm,
        target_rpm,
        motor_status,
        arrow,
        telemetry_online ? "true" : "false",
        telemetry_age_ms);

    if (n < 0 || n >= (int)sizeof(json)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t telemetry_sse_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    while (1) {
        int target_rpm = gTargetRpm;
        int actual_rpm = gActualRpm;
        int motor_status = gMotorStatus;
        int arrow = gArrowState;
        int now_ms = (int)(esp_timer_get_time() / 1000ULL);
        int telemetry_last_ms = gTelemetryLastMs;
        int telemetry_age_ms = (telemetry_last_ms <= now_ms) ? (now_ms - telemetry_last_ms) : 0;
        bool telemetry_online = (telemetry_last_ms != 0) && (telemetry_age_ms <= 1000);

        char event[224];
        int n = snprintf(
            event,
            sizeof(event),
            "event: rpm\ndata: {\"actual_rpm\":%d,\"target_rpm\":%d,\"motor_status\":%d,\"arrow\":%d,\"online\":%s,\"age_ms\":%d}\n\n",
            actual_rpm,
            target_rpm,
            motor_status,
            arrow,
            telemetry_online ? "true" : "false",
            telemetry_age_ms);

        if (n < 0 || n >= (int)sizeof(event)) {
            return ESP_FAIL;
        }

        esp_err_t err = httpd_resp_send_chunk(req, event, n);
        if (err != ESP_OK) {
            return err;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t control_post_handler(httpd_req_t *req)
{
    char body[320];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload length");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read payload");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    bool strip_on = false;
    if (json_read_bool(body, "strip_on", &strip_on)) {
        if (!strip_on) {
            if (gBrightness > 0) {
                gStripBrightnessRestore = gBrightness;
            }
            gStripOn = false;
            gBrightness = 0;
        } else {
            gStripOn = true;
            if (gBrightness == 0) {
                int restore = gStripBrightnessRestore;
                if (restore == 0) {
                    restore = 15;
                }
                gBrightness = restore;
            }
        }
    }

    int brightness = 0;
    if (json_read_int(body, "brightness", &brightness)) {
        if (brightness < 0) brightness = 0;
        if (brightness > 31) brightness = 31;
        if (brightness > 0) {
            gStripBrightnessRestore = brightness;
        }

        if (gStripOn) {
            gBrightness = brightness;
        } else {
            gBrightness = 0;
        }
    }

    int rotation_period_us = 0;
    if (json_read_int(body, "rotation_period_us", &rotation_period_us)) {
        if (rotation_period_us < 1) rotation_period_us = 1;
        gRotationPeriodUs = rotation_period_us;
    }

    int rotation_delay_ppm = 0;
    if (json_read_int(body, "rotation_delay_ppm", &rotation_delay_ppm)) {
        if (rotation_delay_ppm < 0) rotation_delay_ppm = 0;
        if (rotation_delay_ppm > 1000000) rotation_delay_ppm = 1000000;
        gRotationDelayPpm = rotation_delay_ppm;
    }

    bool angle_lock = false;
    if (json_read_bool(body, "angle_lock", &angle_lock)) {
        gAngleLockEnabled = angle_lock;
    }

    int image_index = 0;
    if (json_read_int(body, "image_index", &image_index)) {
        if (image_index < 0) image_index = 0;
#if POV_IMAGE_COUNT > 0
        image_index = image_index % POV_IMAGE_COUNT;
#else
        image_index = 0;
#endif
        gActiveImageIndex = image_index;
        gMode = 2 + image_index;
    }

    int target_rpm = 0;
    if (json_read_int(body, "target_rpm", &target_rpm)) {
        if (target_rpm < 0) target_rpm = 0;
        if (target_rpm > 0 && target_rpm < 300) target_rpm = 300;
        if (target_rpm > 1500) target_rpm = 1500;
        gTargetRpm = target_rpm;
        espnowSendControl();
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t image_post_handler(httpd_req_t *req)
{
    if (req->content_len != (int)POV_IMAGE_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid image payload size");
        return ESP_FAIL;
    }

    init_runtime_image_buffers();
    if (!gRuntimeImageBuffers[0] && !gRuntimeImageBuffers[1]) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Image buffer allocation failed");
        return ESP_FAIL;
    }

    int current = gRuntimeImageActive;
    int write_index = 0;

    if (gRuntimeImageBuffers[0] && gRuntimeImageBuffers[1]) {
        write_index = (current == 0) ? 1 : 0;
    } else if (gRuntimeImageBuffers[0]) {
        write_index = 0;
    } else {
        write_index = 1;
    }

    if (write_index < 0 || write_index > 1) {
        write_index = 0;
    }

    unsigned char *dst = gRuntimeImageBuffers[write_index];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, (char *)dst + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read image payload");
            return ESP_FAIL;
        }
        received += r;
    }

    gRuntimeImageBytes = POV_IMAGE_BYTES;
    gRuntimeImageActive = write_index;
    gMode = 2;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t image_delete_handler(httpd_req_t *req)
{
    gRuntimeImageActive = -1;
    gRuntimeImageBytes = 0;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 24;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_err_handler));

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root_uri));

    httpd_uri_t workbench_uri = {
        .uri = "/workbench.html",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &workbench_uri));

    httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &health_uri));

    httpd_uri_t android_gen204_uri = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = android_generate_204_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &android_gen204_uri));

    httpd_uri_t android_gen204_alt_uri = {
        .uri = "/gen_204",
        .method = HTTP_GET,
        .handler = android_generate_204_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &android_gen204_alt_uri));

    httpd_uri_t apple_hotspot_uri = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = apple_hotspot_detect_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &apple_hotspot_uri));

    httpd_uri_t ms_ncsi_uri = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = microsoft_ncsi_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ms_ncsi_uri));

    httpd_uri_t ms_connecttest_uri = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = microsoft_connecttest_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ms_connecttest_uri));

    httpd_uri_t memory_uri = {
        .uri = "/memory",
        .method = HTTP_GET,
        .handler = memory_get_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &memory_uri));

    httpd_uri_t state_uri = {
        .uri = "/state",
        .method = HTTP_GET,
        .handler = state_get_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &state_uri));

    httpd_uri_t telemetry_events_uri = {
        .uri = "/events",
        .method = HTTP_GET,
        .handler = telemetry_sse_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &telemetry_events_uri));

    httpd_uri_t control_uri = {
        .uri = "/control",
        .method = HTTP_POST,
        .handler = control_post_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &control_uri));

    httpd_uri_t image_post_uri = {
        .uri = "/image",
        .method = HTTP_POST,
        .handler = image_post_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &image_post_uri));

    httpd_uri_t image_delete_uri = {
        .uri = "/image",
        .method = HTTP_DELETE,
        .handler = image_delete_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &image_delete_uri));

    httpd_uri_t options_uri = {
        .uri = "/*",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &options_uri));

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}

static void init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed (%s). Using fallback page.", esp_err_to_name(err));
        return;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: %u / %u bytes used", (unsigned)used, (unsigned)total);
    }
}

void wirelessInit(void)
{
    wifiInit();
    espnowDisplayInit();
    init_spiffs();
    start_web_server();
}
