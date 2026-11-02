#pragma once
#include <cstdint>
#include <map>
#include <string>

typedef int esp_err_t;
typedef int nvs_handle;
#define ESP_OK 0
#define ESP_FAIL -1
#define NVS_READWRITE 1

// Survives a simulated reboot, exactly like the real thing survives a real one
extern std::map<std::string, uint32_t> testNvs;
extern bool testNvsWorks;

inline esp_err_t nvs_flash_init() { return ESP_OK; }
inline esp_err_t nvs_open(const char *, int, nvs_handle *out) { *out = 1; return testNvsWorks ? ESP_OK : ESP_FAIL; }
inline esp_err_t nvs_commit(nvs_handle) { return ESP_OK; }

inline esp_err_t nvs_get_u8(nvs_handle, const char *key, uint8_t *out) {
  auto it = testNvs.find(key);
  if (it == testNvs.end()) return ESP_FAIL;
  *out = (uint8_t)it->second;
  return ESP_OK;
}
inline esp_err_t nvs_get_u32(nvs_handle, const char *key, uint32_t *out) {
  auto it = testNvs.find(key);
  if (it == testNvs.end()) return ESP_FAIL;
  *out = it->second;
  return ESP_OK;
}
inline esp_err_t nvs_set_u8(nvs_handle, const char *key, uint8_t v) { testNvs[key] = v; return ESP_OK; }
inline esp_err_t nvs_set_u32(nvs_handle, const char *key, uint32_t v) { testNvs[key] = v; return ESP_OK; }
