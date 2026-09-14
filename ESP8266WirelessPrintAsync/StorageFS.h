#pragma once

#include "FileWrapper.h"

class StorageFS {
  private:
    static bool hasSD;

  public:
    inline static void begin() {
      hasSD = SD_MMC.begin("/sdcard", true);   // ESP32-CAM slot, 1-bit mode frees GPIO4/12/13
    }

    inline static bool isActive() {
      return hasSD;
    }

    inline static String getActiveFS() {
      return hasSD ? "SD" : "NO FS";
    }

    inline static unsigned int getMaxPathLength() {
      return 255;
    }

    inline static bool exists(const String path) {
      return hasSD && path.length() > 1 && path.startsWith("/") && SD_MMC.exists(path);
    }

    inline static uint64_t totalBytes() {
      return hasSD ? SD_MMC.totalBytes() : 0;
    }

    inline static uint64_t freeBytes() {
      return hasSD ? SD_MMC.totalBytes() - SD_MMC.usedBytes() : 0;
    }

    static FileWrapper open(const String path, const char *openMode = "r");
    static void remove(const String filename);
};

extern StorageFS storageFS;
