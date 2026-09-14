#pragma once

#define FS_NO_GLOBALS // allow spiffs to coexist with SD card, define BEFORE including FS.h
#include <FS.h>
#include <SPIFFS.h>
#define FORMAT_SPIFFS_IF_FAILED true
#include <SD_MMC.h>
using fs::File;

class FileWrapper : public Stream {
  friend class StorageFS;

  private:
    File sdFile;
    fs::File fsFile;

  public:
    // Print methods
    virtual size_t write(uint8_t datum);
    virtual size_t write(const uint8_t *buf, size_t size);

    // Stream methods
    virtual void flush();
    virtual int available();
    virtual int peek();
    virtual int read();

    inline operator bool() {
      return sdFile || fsFile;
    }

    String name();
    uint32_t size();
    size_t read(uint8_t *buf, size_t size);
    String readStringUntil(char eol);
    void close();

    inline bool isDirectory() {
      if (sdFile)
        return sdFile.isDirectory();

      return fsFile ? fsFile.isDirectory() : false;
    }

    FileWrapper openNextFile();
};
