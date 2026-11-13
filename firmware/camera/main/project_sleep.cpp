#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "project_settings.h"
#include "project_sleep.h"
#include "project_camera_manager.h"

// A held-active PIR must first clear, otherwise level wake would reboot forever.
// Clearing wakes only long enough to arm the next motion wake (no camera/Wi-Fi).
RTC_DATA_ATTR bool waitingForPirClear = false;

static void outputOff(int pin, int activeLevel, bool hold) {
  if (pin < 0)
    return;
  digitalWrite(pin, !activeLevel);
  pinMode(pin, OUTPUT);
  if (hold)
    gpio_hold_en((gpio_num_t)pin);
  else
    gpio_hold_dis((gpio_num_t)pin);
}

void powerDownProjectCamera() {
  shutdownCameraManager();
}

void sleepProjectUntilMotion() {
  powerDownProjectCamera();
  WiFi.mode(WIFI_OFF);
  outputOff(WHITE_LED_PIN, WHITE_LED_ACTIVE_LEVEL, true);
  outputOff(BUZZER_PIN, BUZZER_ACTIVE_LEVEL, true);
  outputOff(RED_LED_PIN, RED_LED_ACTIVE_LEVEL, true);
  gpio_deep_sleep_hold_en();
  waitingForPirClear = digitalRead(PIR_PIN) == PIR_ACTIVE_LEVEL;
  int wakeLevel = waitingForPirClear ? !PIR_ACTIVE_LEVEL : PIR_ACTIVE_LEVEL;
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_err_t result = esp_sleep_enable_ext0_wakeup((gpio_num_t)PIR_PIN, wakeLevel);
  if (result != ESP_OK) {
    Serial.printf("PIR wake configuration failed: %d. Check GPIO.\n", result);
    while (true)
      delay(1000);
  }
  // Bias inactive if disconnected; a powered PIR actively drives this input.
  if (PIR_ACTIVE_LEVEL == HIGH) {
    rtc_gpio_pullup_dis((gpio_num_t)PIR_PIN);
    rtc_gpio_pulldown_en((gpio_num_t)PIR_PIN);
  } else {
    rtc_gpio_pulldown_dis((gpio_num_t)PIR_PIN);
    rtc_gpio_pullup_en((gpio_num_t)PIR_PIN);
  }
  Serial.println(waitingForPirClear
                     ? "DEEP SLEEP: waiting for PIR to clear, then rearm."
                     : "DEEP SLEEP: armed for PIR motion. Camera and Wi-Fi stopped.");
  Serial.flush();
  esp_deep_sleep_start();
}

void prepareProjectWake() {
  // Release retained outputs even when switching back to always-awake test mode.
  outputOff(WHITE_LED_PIN, WHITE_LED_ACTIVE_LEVEL, false);
  outputOff(BUZZER_PIN, BUZZER_ACTIVE_LEVEL, false);
  outputOff(RED_LED_PIN, RED_LED_ACTIVE_LEVEL, false);
  gpio_deep_sleep_hold_dis();
  if (PIR_PIN >= 0 && rtc_gpio_is_valid_gpio((gpio_num_t)PIR_PIN))
    rtc_gpio_deinit((gpio_num_t)PIR_PIN);
  if (!LOW_POWER_MODE)
    return;
  if (PIR_PIN < 0 || !rtc_gpio_is_valid_gpio((gpio_num_t)PIR_PIN)) {
    Serial.println("Low-power mode needs a connected RTC-capable PIR GPIO. GPIO 1 is suitable.");
    while (true)
      delay(1000);
  }
  pinMode(PIR_PIN, INPUT);
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    if (waitingForPirClear) {
      Serial.println("PIR cleared; rearming without camera or Wi-Fi startup.");
      sleepProjectUntilMotion();
    }
    Serial.println("PIR WAKE: capture first, then connect Wi-Fi and upload.");
    return;
  }
  // Cold start/reset: warm up only once, not after every PIR wake.
  Serial.printf("LOW POWER MODE: PIR warmup %lu seconds; then deep sleep.\n", PIR_WARMUP_MS / 1000);
  delay(PIR_WARMUP_MS);
  sleepProjectUntilMotion();
}
