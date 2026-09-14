#pragma once

#define FS_NO_GLOBALS
#include <FS.h>
#include <SD_MMC.h>
using fs::File;

class FileWrapper : public Stream {
  friend class StorageFS;

  private:
    File sdFile;

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
      return sdFile;
    }

    String name();
    uint32_t size();
    uint32_t lastWrite();
    size_t read(uint8_t *buf, size_t size);
    String readStringUntil(char eol);
    void close();

    inline bool isDirectory() {
      return sdFile ? sdFile.isDirectory() : false;
    }

    FileWrapper openNextFile();
};
