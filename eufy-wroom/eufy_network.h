#include <stdbool.h>
#pragma once
#include <stddef.h>
#include "esp_err.h"
void eufy_network_start(void);
esp_err_t eufy_network_configure(const char *ssid, const char *password);
void eufy_network_status(char *buffer, size_t size);
bool eufy_network_token_valid(const char *provided);
