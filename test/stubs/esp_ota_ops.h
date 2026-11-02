#pragma once
#include "esp_partition.h"
#include "nvs.h"

typedef struct { char version[32]; } esp_app_desc_t;

extern esp_partition_t testApp0, testApp1;
extern esp_partition_t *testRunning;    // Which slot the processor actually came up in
extern esp_partition_t *testBootSet;    // Which slot the loader has been told to use next
extern bool testApp0Written, testApp1Written;

inline const esp_partition_t *esp_ota_get_running_partition() { return testRunning; }
inline const esp_partition_t *esp_ota_get_boot_partition() { return testBootSet; }
inline const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *) {
  return testRunning == &testApp0 ? &testApp1 : &testApp0;
}
inline esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) {
  testBootSet = (esp_partition_t *)p;
  return ESP_OK;
}
inline esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *) {
  if (p == &testApp0) return testApp0Written ? ESP_OK : ESP_FAIL;
  if (p == &testApp1) return testApp1Written ? ESP_OK : ESP_FAIL;
  return ESP_FAIL;
}
