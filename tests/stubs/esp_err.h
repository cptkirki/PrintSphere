#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 1, ESP_ERR_INVALID_ARG = 2,
  ESP_ERR_INVALID_STATE = 3, ESP_ERR_NOT_FOUND = 4, ESP_ERR_NVS_NO_FREE_PAGES = 5,
  ESP_ERR_NVS_NEW_VERSION_FOUND = 6, ESP_ERR_TIMEOUT = 7;
inline const char* esp_err_to_name(int) { return "test error"; }
