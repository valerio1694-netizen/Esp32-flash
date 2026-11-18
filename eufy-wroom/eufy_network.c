/*
 * Eufy ESP32-WROOM: station-first Wi-Fi, AP only while disconnected.
 * Credentials are provisioned locally and stored in NVS, never at build time.
 * Does not access or reset the robot/GD32 MCU.
 * ESP-IDF 6.0.2, original ESP32.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "wifi.h"
#include "eufy_network.h"

#define NVS_NAMESPACE "eufy_sta"
#define NVS_SSID "ssid"
#define NVS_PASSWORD "password"
#define AUTH_NAMESPACE "eufy_auth"
#define AUTH_KEY "token"

static const char *TAG = "eufy_network";
static volatile bool connected = false;
static bool configured = false;
static char ssid_name[33] = "";
static esp_netif_t *sta_netif = NULL;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
        ESP_LOGW(TAG, "STA disconnected; fallback AP will be enabled");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        connected = true;
        ip_event_got_ip_t *info = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA IP " IPSTR, IP2STR(&info->ip_info.ip));
    }
}


/* Keep OTA authorization available when the SoftAP is disabled in STA mode.
   Credentials are stored only on the device (NVS), not in public source. */
bool eufy_network_token_valid(const char *provided)
{
    if (provided == NULL || strlen(provided) < 8 || strlen(provided) > 63)
        return false;
    nvs_handle_t h;
    if (nvs_open(AUTH_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return false;
    char stored[65] = {0};
    size_t len = sizeof(stored);
    esp_err_t err = nvs_get_str(h, AUTH_KEY, stored, &len);
    nvs_close(h);
    if (err != ESP_OK) return false;
    size_t got = strlen(provided), expected = strlen(stored);
    if (got != expected) return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < got; ++i)
        difference |= (unsigned char)provided[i] ^ (unsigned char)stored[i];
    memset(stored, 0, sizeof(stored));
    return difference == 0;
}

static void ensure_ota_token(const char *ap_password)
{
    if (ap_password == NULL || strlen(ap_password) < 8 ||
        strlen(ap_password) > 63) {
        ESP_LOGE(TAG, "Cannot provision OTA token: no valid AP credential");
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(AUTH_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open OTA token store: %s", esp_err_to_name(err));
        return;
    }
    char existing[65] = {0};
    size_t length = sizeof(existing);
    err = nvs_get_str(h, AUTH_KEY, existing, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_set_str(h, AUTH_KEY, ap_password);
        if (err == ESP_OK) err = nvs_commit(h);
        if (err != ESP_OK)
            ESP_LOGE(TAG, "Cannot persist OTA token: %s", esp_err_to_name(err));
        else
            ESP_LOGI(TAG, "OTA token stored in NVS (value not logged)");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA token cannot be read: %s", esp_err_to_name(err));
    }
    memset(existing, 0, sizeof(existing));
    nvs_close(h);
}

static void reconnect_task(void *arg)
{
    bool ap_enabled = true;
    int retry_ticks = 0;
    (void)arg;
    while (true) {
        /* Change Wi-Fi mode outside the event callback. This leaves SWD/HTTP running. */
        if (connected && ap_enabled) {
            esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
            if (err == ESP_OK) {
                ap_enabled = false;
                ESP_LOGI(TAG, "Connected to home Wi-Fi; fallback AP disabled");
            } else ESP_LOGW(TAG, "Cannot disable AP: %s", esp_err_to_name(err));
        } else if (!connected && !ap_enabled) {
            esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
            if (err == ESP_OK) {
                ap_enabled = true;
                ESP_LOGW(TAG, "Home Wi-Fi lost; fallback AP enabled");
            } else ESP_LOGW(TAG, "Cannot enable fallback AP: %s", esp_err_to_name(err));
        }
        if (configured && !connected && ++retry_ticks >= 5) {
            retry_ticks = 0;
            esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK && result != ESP_ERR_WIFI_CONN)
                ESP_LOGW(TAG, "STA connect retry: %s", esp_err_to_name(result));
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static esp_err_t apply_sta(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL)
        return ESP_ERR_INVALID_ARG;
    size_t sl = strlen(ssid), pl = strlen(password);
    if (sl < 1 || sl > 32 || pl < 8 || pl > 63)
        return ESP_ERR_INVALID_ARG;

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, sl);
    memcpy(config.sta.password, password, pl);
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) return err;

    connected = false;
    snprintf(ssid_name, sizeof(ssid_name), "%s", ssid);
    configured = true;
    err = esp_wifi_connect();
    /* "already connecting" is not a provisioning failure. */
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
        ESP_LOGW(TAG, "Initial STA connect: %s", esp_err_to_name(err));
    return ESP_OK;
}

esp_err_t eufy_network_configure(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) return ESP_ERR_INVALID_ARG;
    size_t sl = strlen(ssid), pl = strlen(password);
    if (sl < 1 || sl > 32 || pl < 8 || pl > 63)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_PASSWORD, password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    return apply_sta(ssid, password);
}

void eufy_network_status(char *buffer, size_t size)
{
    esp_netif_ip_info_t ip = {0};
    char address[20] = "0.0.0.0";
    if (connected && sta_netif &&
        esp_netif_get_ip_info(sta_netif, &ip) == ESP_OK) {
        snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.ip));
    }
    char safe_ssid[33];
    snprintf(safe_ssid, sizeof(safe_ssid), "%s", ssid_name);
    for (char *p = safe_ssid; *p; ++p)
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20)
            *p = '_';
    snprintf(buffer, size,
        "{\"mode\":\"AP+STA\",\"fallback_ap\":\"192.168.4.1\","
        "\"sta_configured\":%s,\"sta_connected\":%s,"
        "\"sta_ssid\":\"%s\",\"sta_ip\":\"%s\"}",
        configured ? "true" : "false",
        connected ? "true" : "false", safe_ssid, address);
}

void eufy_network_start(void)
{
    /* The pinned upstream firmware has already started the SoftAP here. */
    if (sta_netif != NULL) return;
    sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        ESP_LOGE(TAG, "Failed to create station interface; AP remains active");
        return;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "APSTA mode failed: %s", esp_err_to_name(err));
        return;
    }

    /* Persist the AP password currently running, so later OTA builds
       do not rotate the fallback AP credential. The first upgrade from the
       old firmware may change it once (see build's wifi-credentials.txt). */
    wifi_config_t current_ap = {0};
    if (esp_wifi_get_config(WIFI_IF_AP, &current_ap) == ESP_OK) {
        char ap_ssid[33] = {0};
        char ap_pass[65] = {0};
        memcpy(ap_ssid, current_ap.ap.ssid, 32);
        memcpy(ap_pass, current_ap.ap.password, 64);
        ensure_ota_token(ap_pass);
        err = wifi_save_ap_config(ap_ssid, ap_pass, current_ap.ap.channel);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "AP credential persist failed: %s", esp_err_to_name(err));
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        char stored_ssid[33] = {0};
        char stored_password[65] = {0};
        size_t sl = sizeof(stored_ssid);
        size_t pl = sizeof(stored_password);
        esp_err_t a = nvs_get_str(h, NVS_SSID, stored_ssid, &sl);
        esp_err_t b = nvs_get_str(h, NVS_PASSWORD, stored_password, &pl);
        nvs_close(h);
        if (a == ESP_OK && b == ESP_OK) {
            err = apply_sta(stored_ssid, stored_password);
            if (err != ESP_OK)
                ESP_LOGW(TAG, "Saved STA config not applied: %s", esp_err_to_name(err));
        }
    }
    if (xTaskCreate(reconnect_task, "eufy_sta_retry", 3072, NULL, 2, NULL) != pdPASS)
        ESP_LOGE(TAG, "Cannot create STA reconnect task");
    ESP_LOGI(TAG, "Station-first Wi-Fi initialized; fallback AP only when disconnected");
}
