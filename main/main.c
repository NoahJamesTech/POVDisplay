//#### BLADE CODE #####//

#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
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
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_partition.h"
#include "pov_config.h"
#include "pov_gallery.h"
#include "functions.h"

static const char *TAG = "POVBlade";

#define POV_FIRMWARE_VERSION "1.1.0"

#define POV_IMAGE_BYTES ((size_t)POV_GLOBAL_COLS * (size_t)POV_GLOBAL_LEDS * 3u)

static uint8_t *gRuntimeImageBuffers[2] = {NULL, NULL};
static volatile int gRuntimeImageActive = -1;
static volatile size_t gRuntimeImageBytes = 0;

static void init_runtime_image_buffers(void)
{
    if (gRuntimeImageBuffers[0] && gRuntimeImageBuffers[1]) {
        return;
    }

    if (!gRuntimeImageBuffers[0]) {
        gRuntimeImageBuffers[0] = (uint8_t *)heap_caps_malloc(POV_IMAGE_BYTES, MALLOC_CAP_8BIT);
    }
    if (!gRuntimeImageBuffers[1]) {
        gRuntimeImageBuffers[1] = (uint8_t *)heap_caps_malloc(POV_IMAGE_BYTES, MALLOC_CAP_8BIT);
    }

    if (!gRuntimeImageBuffers[0] || !gRuntimeImageBuffers[1]) {
        ESP_LOGE(TAG, "Failed to allocate runtime image buffers (%u bytes each)", (unsigned)POV_IMAGE_BYTES);
        if (gRuntimeImageBuffers[0]) {
            free(gRuntimeImageBuffers[0]);
            gRuntimeImageBuffers[0] = NULL;
        }
        if (gRuntimeImageBuffers[1]) {
            free(gRuntimeImageBuffers[1]);
            gRuntimeImageBuffers[1] = NULL;
        }
    }
}

static const char *WORKBENCH_FALLBACK_HTML =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>POV Workbench</title></head><body style='font-family:sans-serif;padding:16px'>"
    "<h2>POV Workbench file not found</h2>"
    "<p>Flash <code>utilities/workbench.html</code> to SPIFFS as <code>/spiffs/workbench.html</code>.</p>"
    "</body></html>";

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

static esp_err_t version_get_handler(httpd_req_t *req)
{
    char json[96];
    int n = snprintf(json, sizeof(json),
                     "{\"ok\":true,\"firmware_version\":\"%s\"}",
                     POV_FIRMWARE_VERSION);
    if (n < 0 || n >= (int)sizeof(json)) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
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
    uint8_t brightness = gBrightness;
    uint32_t rotation_period_us = gRotationPeriodUs;
    uint16_t image_index = gActiveImageIndex;
    uint16_t target_rpm = gTargetRpm;
    uint16_t actual_rpm = gActualRpm;
    uint8_t motor_status = gMotorStatus;
    uint8_t arrow = gArrowState;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t telemetry_last_ms = gTelemetryLastMs;
    uint32_t telemetry_age_ms = (telemetry_last_ms <= now_ms) ? (now_ms - telemetry_last_ms) : 0;
    bool telemetry_online = (telemetry_last_ms != 0U) && (telemetry_age_ms <= 1000U);

    char json[544];
    int n = snprintf(
        json,
        sizeof(json),
        "{\"ok\":true,\"control\":{\"strip_on\":%s,\"brightness\":%u,\"rotation_period_us\":%u,\"image_index\":%u,\"target_rpm\":%u},\"telemetry\":{\"actual_rpm\":%u,\"target_rpm\":%u,\"motor_status\":%u,\"arrow\":%u,\"online\":%s,\"age_ms\":%u}}",
        gStripOn ? "true" : "false",
        (unsigned)brightness,
        (unsigned)rotation_period_us,
        (unsigned)image_index,
        (unsigned)target_rpm,
        (unsigned)actual_rpm,
        (unsigned)target_rpm,
        (unsigned)motor_status,
        (unsigned)arrow,
        telemetry_online ? "true" : "false",
        (unsigned)telemetry_age_ms);

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
        uint16_t target_rpm = gTargetRpm;
        uint16_t actual_rpm = gActualRpm;
        uint8_t motor_status = gMotorStatus;
        uint8_t arrow = gArrowState;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        uint32_t telemetry_last_ms = gTelemetryLastMs;
        uint32_t telemetry_age_ms = (telemetry_last_ms <= now_ms) ? (now_ms - telemetry_last_ms) : 0;
        bool telemetry_online = (telemetry_last_ms != 0U) && (telemetry_age_ms <= 1000U);

        char event[224];
        int n = snprintf(
            event,
            sizeof(event),
            "event: rpm\ndata: {\"actual_rpm\":%u,\"target_rpm\":%u,\"motor_status\":%u,\"arrow\":%u,\"online\":%s,\"age_ms\":%u}\n\n",
            (unsigned)actual_rpm,
            (unsigned)target_rpm,
            (unsigned)motor_status,
            (unsigned)arrow,
            telemetry_online ? "true" : "false",
            (unsigned)telemetry_age_ms);

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
        gStripOn = strip_on;
    }

    int brightness = 0;
    if (json_read_int(body, "brightness", &brightness)) {
        if (brightness < 0) brightness = 0;
        if (brightness > 31) brightness = 31;
        gBrightness = (uint8_t)brightness;
    }

    int rotation_period_us = 0;
    if (json_read_int(body, "rotation_period_us", &rotation_period_us)) {
        if (rotation_period_us < 1) rotation_period_us = 1;
        gRotationPeriodUs = (uint32_t)rotation_period_us;
    }

    int image_index = 0;
    if (json_read_int(body, "image_index", &image_index)) {
        if (image_index < 0) image_index = 0;
#if POV_IMAGE_COUNT > 0
        image_index = image_index % POV_IMAGE_COUNT;
#else
        image_index = 0;
#endif
        gActiveImageIndex = (uint16_t)image_index;
        gMode = (uint8_t)(2 + image_index);
    }

    int target_rpm = 0;
    if (json_read_int(body, "target_rpm", &target_rpm)) {
        if (target_rpm < 0) target_rpm = 0;
        if (target_rpm > 0 && target_rpm < 300) target_rpm = 300;
        if (target_rpm > 1500) target_rpm = 1500;
        gTargetRpm = (uint16_t)target_rpm;
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
    if (!gRuntimeImageBuffers[0] || !gRuntimeImageBuffers[1]) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Image buffer allocation failed");
        return ESP_FAIL;
    }

    int current = gRuntimeImageActive;
    int write_index = (current == 0) ? 1 : 0;
    if (write_index < 0 || write_index > 1) {
        write_index = 0;
    }

    uint8_t *dst = gRuntimeImageBuffers[write_index];
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

    httpd_uri_t version_uri = {
        .uri = "/version",
        .method = HTTP_GET,
        .handler = version_get_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &version_uri));

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

// Change this as needed

#define DOTSTAR_DATA_GPIO  1  // -> DI
#define DOTSTAR_CLK_GPIO   2  // -> CI
#define DOTSTAR_CS_GPIO    13  // CS for oscilloscope timing measurement (directly usable, no connection to strip needed)
#define BLADE_LEDS          (DOTSTAR_NUM_LEDS / 2)   // 36

#define RENDER_POV_IMAGE(IMAGE_PTR, BRIGHTNESS031, ROTATION_US) do {                              \
    uint32_t imageCols = POV_GLOBAL_COLS;                                                        \
    uint32_t imageLeds = POV_GLOBAL_LEDS;                                                        \
    uint32_t rotationPeriodUs = (ROTATION_US);                                                   \
    uint8_t brightness031 = (BRIGHTNESS031);                                                     \
    if (imageCols < 2) imageCols = 2;                                                            \
    if (imageLeds < 1) imageLeds = 1;                                                            \
    if (rotationPeriodUs < 1) rotationPeriodUs = 1;                                              \
    if (brightness031 > 31) brightness031 = 31;                                                  \
                                                                                                 \
    uint32_t positionInRotation = (uint32_t)(elapsed % rotationPeriodUs);                       \
    uint32_t colA = (positionInRotation * imageCols) / rotationPeriodUs;                        \
    if (colA >= imageCols) colA = imageCols - 1;                                                 \
    uint32_t colB = (colA + (imageCols / 2)) % imageCols;                                        \
                                                                                                 \
    uint32_t ledCount = (imageLeds < BLADE_LEDS) ? imageLeds : BLADE_LEDS;                      \
                                                                                                 \
    for (uint32_t i = 0; i < ledCount; i++) {                                                    \
        uint32_t srcLedA = (imageLeds - 1) - i;                                                  \
        dotstarSetPixel(i, brightness031,                                                        \
            (IMAGE_PTR)[colA][srcLedA][0],                                                       \
            (IMAGE_PTR)[colA][srcLedA][1],                                                       \
            (IMAGE_PTR)[colA][srcLedA][2]);                                                      \
        dotstarSetPixel(i + BLADE_LEDS, brightness031,                                           \
            (IMAGE_PTR)[colB][i][0],                                                             \
            (IMAGE_PTR)[colB][i][1],                                                             \
            (IMAGE_PTR)[colB][i][2]);                                                            \
    }                                                                                            \
                                                                                                 \
    for (uint32_t i = ledCount; i < BLADE_LEDS; i++) {                                           \
        dotstarSetPixel(i, 0, 0, 0, 0);                                                          \
        dotstarSetPixel(i + BLADE_LEDS, 0, 0, 0, 0);                                             \
    }                                                                                            \
} while (0)

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

        uint8_t brightness = gBrightness;
        uint32_t rotationPeriodUs = gRotationPeriodUs;
        int64_t now = esp_timer_get_time();
        int64_t elapsed = now - startTime;

        int runtimeIndex = gRuntimeImageActive;
        if (runtimeIndex >= 0 && runtimeIndex <= 1 &&
            gRuntimeImageBytes == POV_IMAGE_BYTES &&
            gRuntimeImageBuffers[runtimeIndex] != NULL) {
            const uint8_t *img = gRuntimeImageBuffers[runtimeIndex];
            uint32_t imageCols = POV_GLOBAL_COLS;
            uint32_t imageLeds = POV_GLOBAL_LEDS;
            uint32_t rotationUs = rotationPeriodUs;
            uint8_t bright = brightness;

            if (imageCols < 2) imageCols = 2;
            if (imageLeds < 1) imageLeds = 1;
            if (rotationUs < 1) rotationUs = 1;
            if (bright > 31) bright = 31;

            uint32_t positionInRotation = (uint32_t)(elapsed % rotationUs);
            uint32_t colA = (positionInRotation * imageCols) / rotationUs;
            if (colA >= imageCols) colA = imageCols - 1;
            uint32_t colB = (colA + (imageCols / 2)) % imageCols;
            uint32_t ledCount = (imageLeds < BLADE_LEDS) ? imageLeds : BLADE_LEDS;

            for (uint32_t i = 0; i < ledCount; i++) {
                uint32_t srcLedA = (imageLeds - 1) - i;
                size_t baseA = ((size_t)colA * imageLeds + srcLedA) * 3u;
                size_t baseB = ((size_t)colB * imageLeds + i) * 3u;

                dotstarSetPixel(i, bright, img[baseA], img[baseA + 1], img[baseA + 2]);
                dotstarSetPixel(i + BLADE_LEDS, bright, img[baseB], img[baseB + 1], img[baseB + 2]);
            }

            for (uint32_t i = ledCount; i < BLADE_LEDS; i++) {
                dotstarSetPixel(i, 0, 0, 0, 0);
                dotstarSetPixel(i + BLADE_LEDS, 0, 0, 0, 0);
            }
        }
#if POV_IMAGE_COUNT > 0
        else {
        uint32_t imageIndex = ((uint32_t)gActiveImageIndex) % POV_IMAGE_COUNT;
        RENDER_POV_IMAGE(pov_images[imageIndex].data, brightness, rotationPeriodUs);
        }
#else
        else {
            for (uint32_t i = 0; i < DOTSTAR_NUM_LEDS; i++) {
                dotstarSetPixel(i, 0, 0, 0, 0);
            }
        }
#endif

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
    gRotationPeriodUs = POV_GLOBAL_ROTATION_PERIOD_US;
    gTargetRpm = 0;

    // Core 0: Wi-Fi + web server + ESP-NOW
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGI(TAG, "Firmware version: %s", POV_FIRMWARE_VERSION);
    wifiInit();
    espnowDisplayInit();
    init_spiffs();
    start_web_server();

    // Run the LED loop on core 1 so core 0 is free for Wi-Fi/ESP-NOW
    xTaskCreatePinnedToCore(ledTask, "ledTask", 4096, NULL, 24, NULL, 1);
    
}
