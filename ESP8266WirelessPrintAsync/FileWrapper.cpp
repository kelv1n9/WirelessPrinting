#include "FileWrapper.h"
#include "StorageFS.h"

size_t FileWrapper::write(uint8_t b) {
  uint8_t buf[] = { b };
  
  return write(buf, 1);
}

size_t FileWrapper::write(const uint8_t *buf, size_t len) {
  return sdFile ? sdFile.write(buf, len) : 0;
}

void FileWrapper::flush() {
  if (sdFile)
    sdFile.flush();
}

int FileWrapper::available() {
  return sdFile ? sdFile.available() : 0;
}

int FileWrapper::peek() {
  return sdFile ? sdFile.peek() : -1;
}

int FileWrapper::read() {
  return sdFile ? sdFile.read() : -1;
}

String FileWrapper::name() {
  return sdFile ? sdFile.name() : String();
}

uint32_t FileWrapper::size() {
  return sdFile ? sdFile.size() : 0;
}

uint32_t FileWrapper::lastWrite() {
  return sdFile ? (uint32_t)sdFile.getLastWrite() : 0;
}

size_t FileWrapper::read(uint8_t *buf, size_t size) {
  return sdFile ? sdFile.read(buf, size) : 0;
}

String FileWrapper::readStringUntil(char eol) {
  return sdFile ? sdFile.readStringUntil(eol) : String();
}

void FileWrapper::close() {
  if (sdFile) {
    sdFile.close();
    sdFile = File();
  }
}

FileWrapper FileWrapper::openNextFile() {
  FileWrapper fw = FileWrapper();

  if (sdFile)
    fw.sdFile = sdFile.openNextFile();

  return fw;
}
