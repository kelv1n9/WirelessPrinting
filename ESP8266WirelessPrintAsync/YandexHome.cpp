#include "YandexHome.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#define YANDEX_HOST      "https://api.iot.yandex.net"
#define YANDEX_TASK_STACK 10240
#define YANDEX_TIMEOUT    15000
#define YANDEX_JSON_SIZE  16384

static const char YANDEX_ROOT_CA[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDXzCCAkegAwIBAgILBAAAAAABIVhTCKIwDQYJKoZIhvcNAQELBQAwTDEgMB4G\n"
"A1UECxMXR2xvYmFsU2lnbiBSb290IENBIC0gUjMxEzARBgNVBAoTCkdsb2JhbFNp\n"
"Z24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMDkwMzE4MTAwMDAwWhcNMjkwMzE4\n"
"MTAwMDAwWjBMMSAwHgYDVQQLExdHbG9iYWxTaWduIFJvb3QgQ0EgLSBSMzETMBEG\n"
"A1UEChMKR2xvYmFsU2lnbjETMBEGA1UEAxMKR2xvYmFsU2lnbjCCASIwDQYJKoZI\n"
"hvcNAQEBBQADggEPADCCAQoCggEBAMwldpB5BngiFvXAg7aEyiie/QV2EcWtiHL8\n"
"RgJDx7KKnQRfJMsuS+FggkbhUqsMgUdwbN1k0ev1LKMPgj0MK66X17YUhhB5uzsT\n"
"gHeMCOFJ0mpiLx9e+pZo34knlTifBtc+ycsmWQ1z3rDI6SYOgxXG71uL0gRgykmm\n"
"KPZpO/bLyCiR5Z2KYVc3rHQU3HTgOu5yLy6c+9C7v/U9AOEGM+iCK65TpjoWc4zd\n"
"QQ4gOsC0p6Hpsk+QLjJg6VfLuQSSaGjlOCZgdbKfd/+RFO+uIEn8rUAVSNECMWEZ\n"
"XriX7613t2Saer9fwRPvm2L7DWzgVGkWqQPabumDk3F2xmmFghcCAwEAAaNCMEAw\n"
"DgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0OBBYEFI/wS3+o\n"
"LkUkrk1Q+mOai97i3Ru8MA0GCSqGSIb3DQEBCwUAA4IBAQBLQNvAUKr+yAzv95ZU\n"
"RUm7lgAJQayzE4aGKAczymvmdLm6AC2upArT9fHxD4q/c2dKg8dEe3jgr25sbwMp\n"
"jjM5RcOO5LlXbKr8EpbsU8Yt5CRsuZRj+9xTaGdWPoO4zzUhw8lo/s7awlOqzJCK\n"
"6fBdRoyV3XpYKBovHd7NADdBj+1EbddTKJd+82cEHhXXipa0095MJ6RMG3NzdvQX\n"
"mcIfeg7jLQitChws/zyrVQ4PkX4268NXSb7hLi18YIvDQVETI53O9zJrlAGomecs\n"
"Mx86OyXShkDOOyyGeMlhLxS67ttVb9+E7gUJTb0o2HLO02JQZR7rkpeDMdmztcpH\n"
"WD9f\n"
"-----END CERTIFICATE-----\n";

YandexHome yandexHome;

SemaphoreHandle_t YandexHome::mutex = NULL;
TaskHandle_t YandexHome::task = NULL;
volatile YandexHome::Request YandexHome::pending = YandexHome::None;
String YandexHome::token, YandexHome::deviceId, YandexHome::deviceName,
       YandexHome::devices, YandexHome::status;

class Guard {
  public:
    inline Guard(SemaphoreHandle_t mutex) : mutex(mutex) {
      if (mutex)
        xSemaphoreTake(mutex, portMAX_DELAY);
    }

    inline ~Guard() {
      if (mutex)
        xSemaphoreGive(mutex);
    }

  private:
    SemaphoreHandle_t mutex;
};

void YandexHome::begin() {
  mutex = xSemaphoreCreateMutex();

  Preferences preferences;
  preferences.begin("yandex", true);
  token = preferences.getString("token", "");
  deviceId = preferences.getString("device", "");
  deviceName = preferences.getString("name", "");
  preferences.end();

  status = token == "" ? "no token" : "idle";

  xTaskCreatePinnedToCore(run, "yandex", YANDEX_TASK_STACK, NULL, 1, &task, 0);
}

bool YandexHome::hasToken() {
  Guard guard(mutex);

  return token != "";
}

void YandexHome::setToken(const String value) {
  Guard guard(mutex);

  token = value;
  status = value == "" ? "no token" : "token stored";

  Preferences preferences;
  preferences.begin("yandex", false);
  preferences.putString("token", value);
  preferences.end();
}

void YandexHome::forgetToken() {
  setToken("");
  setDevice("", "");
}

String YandexHome::getDeviceId() {
  Guard guard(mutex);

  return deviceId;
}

String YandexHome::getDeviceName() {
  Guard guard(mutex);

  return deviceName;
}

void YandexHome::setDevice(const String id, const String name) {
  Guard guard(mutex);

  deviceId = id;
  deviceName = name;

  Preferences preferences;
  preferences.begin("yandex", false);
  preferences.putString("device", id);
  preferences.putString("name", name);
  preferences.end();
}

bool YandexHome::busy() {
  return pending != None;
}

bool YandexHome::request(const Request what) {
  if (pending != None)
    return false;

  pending = what;
  if (task)
    xTaskNotifyGive(task);

  return true;
}

String YandexHome::getDevices() {
  Guard guard(mutex);

  return devices;
}

String YandexHome::getStatus() {
  Guard guard(mutex);

  return status;
}

bool YandexHome::call(const String path, const String payload, String &body) {
  String bearer;
  {
    Guard guard(mutex);
    if (token == "") {
      status = "no token";
      return false;
    }
    bearer = "Bearer " + token;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Guard guard(mutex);
    status = "no network";
    return false;
  }

  WiFiClientSecure client;
  client.setCACert(YANDEX_ROOT_CA);
  client.setTimeout(YANDEX_TIMEOUT / 1000);

  HTTPClient http;
  http.setTimeout(YANDEX_TIMEOUT);
  if (!http.begin(client, YANDEX_HOST + path)) {
    Guard guard(mutex);
    status = "cannot open connection";
    return false;
  }
  http.addHeader("Authorization", bearer);

  int code;
  if (payload == "")
    code = http.GET();
  else {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(payload);
  }

  if (code > 0)
    body = http.getString();
  http.end();

  Guard guard(mutex);
  if (code == 401 || code == 403)
    status = "token rejected, code " + String(code);
  else if (code < 0)
    status = "connection failed, " + String(HTTPClient::errorToString(code));
  else if (code != 200)
    status = "HTTP " + String(code);
  else
    status = "ok";

  return code == 200;
}

void YandexHome::fetchDevices() {
  String body;
  if (!call("/v1.0/user/info", "", body))
    return;

  DynamicJsonDocument document(YANDEX_JSON_SIZE);
  if (deserializeJson(document, body)) {
    Guard guard(mutex);
    status = "cannot parse the device list";
    return;
  }

  String list;
  for (JsonObject device : document["devices"].as<JsonArray>()) {
    const String id = device["id"].as<String>();
    const String name = device["name"].as<String>();
    const String type = device["type"].as<String>();
    if (id == "" || !type.startsWith("devices.types.socket"))
      continue;
    if (list != "")
      list += "\n";
    list += id + "\t" + name;
  }

  Guard guard(mutex);
  devices = list;
  status = list == "" ? "no sockets in this account" : "ok";
}

void YandexHome::switchTo(const bool on) {
  const String id = getDeviceId();
  if (id == "") {
    Guard guard(mutex);
    status = "no device selected";
    return;
  }

  const String payload = "{\"devices\":[{\"id\":\"" + id + "\",\"actions\":[{"
                         "\"type\":\"devices.capabilities.on_off\","
                         "\"state\":{\"instance\":\"on\",\"value\":" + String(on ? "true" : "false") + "}}]}]}";

  String body;
  if (!call("/v1.0/devices/actions", payload, body))
    return;

  Guard guard(mutex);
  status = body.indexOf("\"status\":\"error\"") == -1 ? (on ? "switched on" : "switched off")
                                                     : "device refused: " + body;
}

void YandexHome::run(void *argument) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    const Request what = pending;
    if (what == DeviceList)
      fetchDevices();
    else if (what == PowerOn)
      switchTo(true);
    else if (what == PowerOff)
      switchTo(false);

    pending = None;
  }
}

void YandexHome::loop() {
}
