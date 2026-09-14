#pragma once

#include "FileWrapper.h"

class StorageFS {
  private:
    static bool hasSD,
                hasSPIFFS;
    static unsigned int maxPathLength;

  public:
    inline static void begin() {
      hasSD = SD_MMC.begin("/sdcard", true);   // ESP32-CAM slot, 1-bit mode frees GPIO4/12/13
      if (hasSD)
        maxPathLength = 255;
      else {
        hasSPIFFS = SPIFFS.begin(true);
        maxPathLength = 11;
      }
    }

    inline static bool activeSD() {
      return hasSD;
    }

    inline static bool activeSPIFFS() {
      return hasSPIFFS;
    }

    inline static bool isActive() {
      return activeSD() || activeSPIFFS();
    }

    inline static String getActiveFS() {
      return activeSD() ? "SD" : (activeSPIFFS() ? "SPIFFS" : "NO FS");
    }

    inline static unsigned int getMaxPathLength() {
      return maxPathLength;
    }

    static FileWrapper open(const String path, const char *openMode = "r");
    static void remove(const String filename);
};

extern StorageFS storageFS;
