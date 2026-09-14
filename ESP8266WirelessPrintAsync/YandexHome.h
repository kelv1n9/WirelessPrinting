#pragma once

#include <Arduino.h>

class YandexHome {
  public:
    enum Request { None, DeviceList, PowerOn, PowerOff };

    static void begin();
    static void loop();

    static bool hasToken();
    static void setToken(const String token);
    static void forgetToken();

    static String getDeviceId();
    static String getDeviceName();
    static void setDevice(const String id, const String name);

    static bool request(const Request what);
    static bool busy();

    static String getDevices();
    static String getStatus();

  private:
    static SemaphoreHandle_t mutex;
    static TaskHandle_t task;
    static volatile Request pending;
    static String token, deviceId, deviceName, devices, status;

    static void run(void *argument);
    static bool call(const String path, const String payload, String &body);
    static void fetchDevices();
    static void switchTo(const bool on);
};

extern YandexHome yandexHome;
