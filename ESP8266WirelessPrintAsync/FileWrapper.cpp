#include "FileWrapper.h"
#include "StorageFS.h"

size_t FileWrapper::write(uint8_t b) {
  uint8_t buf[] = { b };
  
  return write(buf, 1);
}

size_t FileWrapper::write(const uint8_t *buf, size_t len) {
  if (sdFile)
    return sdFile.write(buf, len);
  else if (fsFile)
    return fsFile.write(buf, len);

  return 0;
}

void FileWrapper::flush() {
  if (sdFile)
    return sdFile.flush();
  else if (fsFile)
    return fsFile.flush();
}

int FileWrapper::available() {
  return sdFile ? sdFile.available() : (fsFile ? fsFile.available() : false);
}

int FileWrapper::peek() {
  if (sdFile)
    return sdFile.peek();
  else if (fsFile)
    return fsFile.peek();

  return -1;
}

int FileWrapper::read() {
  if (sdFile)
    return sdFile.read();
  else if (fsFile)
    return fsFile.read();

  return -1;
}

String FileWrapper::name() {
  return sdFile ? sdFile.name() : (fsFile ? fsFile.name() : String());
}

uint32_t FileWrapper::size() {
  if (sdFile)
    return sdFile.size();
  else if (fsFile)
    return fsFile.size();

  return 0;
}

size_t FileWrapper::read(uint8_t *buf, size_t size) {
  if (sdFile)
    return sdFile.read(buf, size);
  else if (fsFile)
    return fsFile.read(buf, size);

  return 0;
}

String FileWrapper::readStringUntil(char eol) {
  return sdFile ? sdFile.readStringUntil(eol) : (fsFile ? fsFile.readStringUntil(eol) : "");
}

void FileWrapper::close() {
  if (sdFile) {
    sdFile.close();
    sdFile = File();
  }
  else if (fsFile) {
    fsFile.close();
    fsFile = fs::File();
  }
}

FileWrapper FileWrapper::openNextFile() {
  FileWrapper fw = FileWrapper();

  if (sdFile)
    fw.sdFile = sdFile.openNextFile();
  else if (fsFile)
    fw.fsFile = fsFile.openNextFile();

  return fw;
}
