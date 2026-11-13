#pragma once
#include <WiFiClientSecure.h>
bool configureProjectTls(WiFiClientSecure &client);
void syncProjectClock();
String projectServerUrl(const char *path);
