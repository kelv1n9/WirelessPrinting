#include "StorageFS.h"

StorageFS storageFS;

bool StorageFS::hasSD, 
     StorageFS::hasSPIFFS;
unsigned int StorageFS::maxPathLength;


FileWrapper StorageFS::open(const String path, const char *openMode) {
  FileWrapper file;

  if (openMode == NULL || openMode[0] == '\0')
    return file;

  if (hasSD)
    file.sdFile = SD_MMC.open(path, openMode);
  else if (hasSPIFFS)
    file.fsFile = SPIFFS.open(path, openMode);

  return file;
}

void StorageFS::remove(const String filename) {
  if (hasSD)
    SD_MMC.remove(filename);
  else if (hasSPIFFS)
    SPIFFS.remove(filename);
}
