#include <Arduino.h>
#include <time.h>
#include "project_settings.h"
#include "project_transport.h"

void syncProjectClock() {
  configTime(0, 0, "time.cloudflare.com", "pool.ntp.org", "time.google.com");
  unsigned long start = millis();
  while (time(nullptr) < 1700000000 && millis() - start < 10000)
    delay(100);
  if (time(nullptr) < 1700000000)
    Serial.println("Clock not synchronized yet; verified HTTPS will wait for valid time.");
}
bool configureProjectTls(WiFiClientSecure &client) {
  if (time(nullptr) < 1700000000)
    return false;
  // Arduino-ESP32 3.3.12 includes the public CA bundle. Never fall back to insecure.
  client.useBuiltinCACertBundle();
  client.setHandshakeTimeout(5);
  return true;
}
String projectServerUrl(const char *path) {
  String url = LOCAL_DASHBOARD_UPLOAD_URL;
  const String suffix = "/api/device/upload";
  if (!url.startsWith("https://") || !url.endsWith(suffix))
    return String();
  url.remove(url.length() - suffix.length());
  return url + path;
}
