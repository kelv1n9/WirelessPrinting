#include "StorageFS.h"

StorageFS storageFS;

bool StorageFS::hasSD;


FileWrapper StorageFS::open(const String path, const char *openMode) {
  FileWrapper file;

  if (hasSD && openMode != NULL && openMode[0] != '\0')
    file.sdFile = SD_MMC.open(path, openMode);

  return file;
}

void StorageFS::remove(const String filename) {
  if (hasSD)
    SD_MMC.remove(filename);
}
