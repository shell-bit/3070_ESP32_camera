#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
bool configureProjectTls(WiFiClientSecure &client);
void syncProjectClock();
void logProjectTransportError(WiFiClientSecure &client, int code);
String projectServerUrl(const char *path);
void addProjectDeviceHeaders(HTTPClient &http);
