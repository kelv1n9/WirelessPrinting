#pragma once

#include <Arduino.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

#define BOOTGUARD_STORE       "bootguard"
#define BOOTGUARD_TRIALS      2         // Boots a fresh image gets before it is given up on
#define BOOTGUARD_HEALTH_MS   60000     // Network has to hold this long for the image to be kept
#define BOOTGUARD_OFFLINE_MS  300000    // Longer than the configuration portal, so that gets its turn
#define BOOTGUARD_QUICK_MS    30000     // A boot shorter than this counts towards the escape
#define BOOTGUARD_QUICK_HITS  3         // Power cycles in a row that force the other slot

static nvs_handle guardStore;
static bool guardOpen;
static bool guardArmed;         // This boot is the one on trial
static bool guardConfirmed;
static bool guardQuickCleared;
static uint32_t guardBootAt;
static uint32_t guardOnlineSince;
static bool guardOnline;     // A plain flag, because a zero stamp cannot be told from a real one
static uint8_t guardTrialsLeft;
static uint8_t guardQuickCount;
static const char *guardVerdict = "not started";

static bool guardOpenStore() {
  if (guardOpen)
    return true;
  nvs_flash_init();               // Arduino calls this too, the second call costs nothing
  guardOpen = nvs_open(BOOTGUARD_STORE, NVS_READWRITE, &guardStore) == ESP_OK;
  return guardOpen;
}

static uint8_t guardGet8(const char *key) {
  uint8_t value = 0;
  if (guardOpen)
    nvs_get_u8(guardStore, key, &value);
  return value;
}

static uint32_t guardGet32(const char *key) {
  uint32_t value = 0;
  if (guardOpen)
    nvs_get_u32(guardStore, key, &value);
  return value;
}

static void guardSet8(const char *key, const uint8_t value) {
  if (!guardOpen)
    return;
  nvs_set_u8(guardStore, key, value);
  nvs_commit(guardStore);
}

static void guardSet32(const char *key, const uint32_t value) {
  if (!guardOpen)
    return;
  nvs_set_u32(guardStore, key, value);
  nvs_commit(guardStore);
}

static void guardForget() {
  guardSet8("trials", 0);
  guardSet32("pending", 0);
  guardTrialsLeft = 0;
}

// An empty or half written slot must never be booted into, or the fall back is the brick.
static bool guardSlotUsable(const esp_partition_t *slot) {
  esp_app_desc_t desc;
  return slot && esp_ota_get_partition_description(slot, &desc) == ESP_OK;
}

static void guardFallBack(const char *why) {
  const esp_partition_t *spare = esp_ota_get_next_update_partition(NULL);
  if (!guardSlotUsable(spare)) {
    guardVerdict = "nowhere to fall back to";
    return;
  }
  guardForget();
  guardSet8("quick", 0);
  if (esp_ota_set_boot_partition(spare) != ESP_OK) {
    guardVerdict = "the other slot refused to be made bootable";
    return;
  }
  (void)why;
  esp_restart();
}

// The first thing setup does. Everything after this point is allowed to be broken.
void bootGuardBegin() {
  guardBootAt = millis();
  if (!guardOpenStore()) {
    guardVerdict = "settings unavailable, running without a net";
    return;
  }

  const esp_partition_t *running = esp_ota_get_running_partition();
  const uint32_t pending = guardGet32("pending");
  guardTrialsLeft = guardGet8("trials");

  guardQuickCount = guardGet8("quick") + 1;
  guardSet8("quick", guardQuickCount);

  // The loader can refuse a damaged image by itself, and then we are already home
  bool spoken = false;
  if (guardTrialsLeft && running && pending && running->address != pending) {
    guardForget();
    guardVerdict = "the loader turned the image down on its own";
    spoken = true;
  }

  if (guardQuickCount >= BOOTGUARD_QUICK_HITS) {
    guardVerdict = "sent back by power cycling";
    guardFallBack("escape");
    return;                       // Only reached when there is no usable spare
  }

  if (!guardTrialsLeft) {
    if (!spoken)
      guardVerdict = "settled";
    return;
  }

  guardArmed = true;
  if (guardTrialsLeft == 1) {     // It already had its boots and never said it was well
    guardVerdict = "out of tries, going back";
    guardFallBack("trial");
    return;
  }

  guardSet8("trials", guardTrialsLeft - 1);
  guardVerdict = "on trial";
}

void bootGuardWatch(const bool online) {
  const uint32_t up = millis() - guardBootAt;

  if (!guardQuickCleared && up >= BOOTGUARD_QUICK_MS) {
    guardQuickCleared = true;
    guardQuickCount = 0;
    guardSet8("quick", 0);
  }

  if (!guardArmed || guardConfirmed)
    return;

  if (!online) {
    guardOnline = false;
    if (up >= BOOTGUARD_OFFLINE_MS) {
      guardVerdict = "no network on a fresh image, going back";
      guardFallBack("offline");
    }
    return;
  }

  if (!guardOnline) {
    guardOnline = true;
    guardOnlineSince = millis();
  }

  #ifdef BOOTGUARD_NEVER_CONFIRM            // A build made only to prove the fall back works
    if (millis() - guardOnlineSince >= BOOTGUARD_HEALTH_MS)
      esp_restart();
  #else
    if (millis() - guardOnlineSince >= BOOTGUARD_HEALTH_MS) {
      guardConfirmed = true;
      guardForget();
      guardVerdict = "confirmed";
    }
  #endif
}

// Called once a firmware image has been written and made bootable, before the reboot.
void bootGuardArm() {
  const esp_partition_t *next = esp_ota_get_boot_partition();
  if (!guardOpenStore() || !next)
    return;
  guardSet32("pending", next->address);
  guardSet8("trials", BOOTGUARD_TRIALS);
}

String bootGuardReport() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *spare = esp_ota_get_next_update_partition(NULL);
  return String(guardVerdict)
       + ", slot " + String(running ? running->label : "?")
       + ", spare " + String(spare ? spare->label : "?")
       + (guardSlotUsable(spare) ? " usable" : " EMPTY")
       + ", tries left " + String(guardTrialsLeft)
       + ", quick boots " + String(guardQuickCount);
}
