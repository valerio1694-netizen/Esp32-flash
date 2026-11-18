/* ESP-IDF 6.0.2: authenticated local Wi-Fi OTA for Eufy WROOM.
   Updates ONLY the ESP32 inactive app partition. NEVER accesses GD32.
   GET /status, POST /upload at port 8081. HTTP within WPA2-protected AP. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "eufy_ota.h"
#include "eufy_network.h"
#include "esp_wifi.h"
#define OTA_PORT 8081
static const char *TAG = "eufy_ota";
static bool authorized(httpd_req_t *r) {
    char token[96] = {0};
    size_t length = httpd_req_get_hdr_value_len(r, "X-OTA-Token");
    if (!length || length >= sizeof(token)) return false;
    if (httpd_req_get_hdr_value_str(r, "X-OTA-Token", token, sizeof(token)) != ESP_OK)
        return false;
    /* The AP password is persisted in NVS after first APSTA boot, so
       a newer OTA image may have a different compiled default password. */
    wifi_config_t ap = {0};
    if (esp_wifi_get_config(WIFI_IF_AP, &ap) != ESP_OK)
        return false;
    char current_password[65] = {0};
    memcpy(current_password, ap.ap.password, 64);
    return strcmp(token, current_password) == 0;
}
static esp_err_t status_handler(httpd_req_t *r) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    char json[256];
    int n = snprintf(json, sizeof(json),
        "{\"ota\":true,\"upload_path\":\"/upload\",\"auth\":\"X-OTA-Token\","
        "\"running\":\"%s\",\"next\":\"%s\",\"next_size\":%lu}",
        running ? running->label : "unknown",
        next ? next->label : "unavailable",
        (unsigned long)(next ? next->size : 0));
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(r, json, n);
}
static esp_err_t wifi_status_handler(httpd_req_t *r) {
    char status[320];
    eufy_network_status(status, sizeof(status));
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, status);
}
static esp_err_t wifi_config_handler(httpd_req_t *r) {
    if (!authorized(r)) {
        httpd_resp_set_status(r, "401 Unauthorized");
        return httpd_resp_sendstr(r, "Invalid X-OTA-Token");
    }
    char ssid[33] = {0};
    char password[65] = {0};
    size_t sl = httpd_req_get_hdr_value_len(r, "X-STA-SSID");
    size_t pl = httpd_req_get_hdr_value_len(r, "X-STA-PASSWORD");
    if (sl == 0 || sl > 32 || pl < 8 || pl > 63 ||
        httpd_req_get_hdr_value_str(r, "X-STA-SSID", ssid, sizeof(ssid)) != ESP_OK ||
        httpd_req_get_hdr_value_str(r, "X-STA-PASSWORD", password, sizeof(password)) != ESP_OK) {
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_sendstr(r, "Require 1..32 byte SSID and 8..63 byte WPA2 passphrase");
    }
    esp_err_t result = eufy_network_configure(ssid, password);
    memset(password, 0, sizeof(password));
    if (result != ESP_OK) {
        httpd_resp_set_status(r, "500 Internal Server Error");
        return httpd_resp_sendstr(r, "Could not configure Wi-Fi station");
    }
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r,
        "{\"saved\":true,\"sta_connecting\":true,\"ap_fallback\":true}");
}
static void reboot_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1800));
    esp_restart();
}
static esp_err_t upload_handler(httpd_req_t *r) {
    if (!authorized(r)) {
        httpd_resp_set_status(r, "401 Unauthorized");
        return httpd_resp_sendstr(r, "X-OTA-Token must match Wi-Fi AP password");
    }
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (!p || r->content_len < 8192 || r->content_len > p->size) {
        httpd_resp_set_status(r, "413 Payload Too Large");
        return httpd_resp_sendstr(r, "Invalid firmware size or OTA partition");
    }
    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(p, r->content_len, &handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
        return ESP_FAIL;
    }
    size_t left = r->content_len;
    char buf[2048];
    while (left) {
        int n = httpd_req_recv(r, buf, left > sizeof(buf) ? sizeof(buf) : left);
        if (n <= 0) { err = ESP_FAIL; break; }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) break;
        left -= (size_t)n;
    }
    if (err != ESP_OK || left != 0) {
        esp_ota_abort(handle);
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA interrupted");
        return ESP_FAIL;
    }
    err = esp_ota_end(handle); /* image integrity verification */
    if (err == ESP_OK) err = esp_ota_set_boot_partition(p);
    if (err != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Image verification failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"result\":\"accepted\",\"reboot\":true}");
    xTaskCreate(reboot_task, "eufy_reboot", 2048, NULL, 4, NULL);
    return ESP_OK;
}
void eufy_ota_server_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = OTA_PORT;
    cfg.max_uri_handlers = 6;
    cfg.stack_size = 6144;
    httpd_handle_t s = NULL;
    if (httpd_start(&s, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OTA on %d", OTA_PORT);
        return;
    }
    httpd_uri_t stat = { .uri="/status", .method=HTTP_GET, .handler=status_handler };
    httpd_uri_t up = { .uri="/upload", .method=HTTP_POST, .handler=upload_handler };
    httpd_uri_t wstatus = {
        .uri="/wifi/status", .method=HTTP_GET, .handler=wifi_status_handler
    };
    httpd_uri_t wconfig = {
        .uri="/wifi/config", .method=HTTP_POST, .handler=wifi_config_handler
    };
    httpd_register_uri_handler(s, &stat);
    httpd_register_uri_handler(s, &up);
    httpd_register_uri_handler(s, &wstatus);
    httpd_register_uri_handler(s, &wconfig);
    /* Only after server has started, mark OTA app as bootable for rollback. */
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "OTA ready on port %d", OTA_PORT);
}
