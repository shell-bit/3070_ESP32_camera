#include <Arduino.h>
#include <time.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_heap_caps.h"
#include "project_settings.h"
#include "project_transport.h"

void syncProjectClock() {
  configTime(0, 0, "time.cloudflare.com", "pool.ntp.org", "time.google.com");
  unsigned long start = millis();
  while (time(nullptr) < 1700000000 && millis() - start < 10000) delay(100);
  if (time(nullptr) < 1700000000)
    Serial.println("Clock not synchronized yet; verified HTTPS will wait for valid time.");
  else {
    time_t current = time(nullptr);
    struct tm utc = {};
    gmtime_r(&current, &utc);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S UTC", &utc);
    Serial.printf("HTTPS clock: %s; Wi-Fi signal: %d dBm; IP: %s\n",
                  timestamp, WiFi.RSSI(), WiFi.localIP().toString().c_str());
  }
  String target = projectServerUrl("");
  if (target.startsWith("https://")) {
    String host = target.substring(8);
    IPAddress address;
    if (WiFi.hostByName(host.c_str(), address))
      Serial.printf("HTTPS target: %s -> %s (hostname retained for TLS)\n", host.c_str(), address.toString().c_str());
    else
      Serial.printf("HTTPS target DNS lookup failed: %s\n", host.c_str());
  }
}
bool configureProjectTls(WiFiClientSecure &client) {
  if (time(nullptr) < 1700000000) return false;
  // Verify the server certificate with the Arduino-ESP32 public CA bundle.
  client.useBuiltinCACertBundle();
  // HTTPClient speaks HTTP/1.1. Keep this array alive for the TLS handshake.
  static const char *protocols[] = {"http/1.1", nullptr};
  client.setAlpnProtocols(protocols);
  client.setHandshakeTimeout(12); // Allow more time on a slow school/mobile network.
  return true;
}
void logProjectTransportError(WiFiClientSecure &client, int code) {
  if (code >= 0) return;
  char message[192] = {};
  int error = client.lastError(message, sizeof(message));
  Serial.printf("HTTPS failure: %s (%d); TLS error: %d; %s\n",
                HTTPClient::errorToString(code).c_str(), code, error, message);
  Serial.printf("Wi-Fi status: %d; RSSI: %d dBm; internal heap: %u bytes; largest block: %u bytes\n",
                int(WiFi.status()), WiFi.RSSI(),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  if (error == 0)
    Serial.println("No TLS error recorded: DNS/TCP connection may have failed before TLS started.");
}
String projectServerUrl(const char *path) {
  String url = LOCAL_DASHBOARD_UPLOAD_URL;
  const String suffix = "/api/device/upload";
  if (!url.startsWith("https://") || !url.endsWith(suffix)) return String();
  url.remove(url.length() - suffix.length());
  return url + path;
}

void addProjectDeviceHeaders(HTTPClient &http) {
  if (DEVICE_UPLOAD_TOKEN[0])
    http.addHeader("Authorization", String("Bearer ") + DEVICE_UPLOAD_TOKEN);
  http.addHeader("X-Device-ID", DEVICE_ID);
}
