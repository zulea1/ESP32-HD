/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file safeboot_main.cpp
 * @brief Entry point for the MINIMAL safe-boot / recovery firmware that lives in the `factory` partition.
 *
 * This is a SEPARATE, small firmware (PlatformIO env:safeboot_hd) - not the full diagnostic app. It brings up
 * Wi-Fi (STA from NVS, else an open rescue AP) and the recovery web portal only: firmware update (writes the
 * main app to ota_0), factory reset, and "reboot to normal app". No CAN/UDS/module code is compiled in, so it
 * stays tiny and can always flash a new main app even if the app partition is broken.
 *
 * It is compiled ONLY in env:safeboot_hd (build_src_filter selects this file + core/safe_boot.cpp +
 * core/async_log.cpp). The full app (env:esp32_hd) excludes this folder and uses its own core/main.cpp.
 */
#include "config.h"
#include "safe_boot.h"
#include <Preferences.h>

// The single global the recovery portal reads for its info JSON. The full app defines this in main.cpp; this
// minimal build must provide its own copy (Wi-Fi credentials themselves are read from NVS by safe_boot.cpp).
TesterSettings settings;

static void loadSettingsForRecovery() {
    memset(&settings, 0, sizeof(settings));
    uint64_t efuseMac = ESP.getEfuseMac();
    snprintf(settings.node_name, sizeof(settings.node_name),
             "ESP32 HD [%02X:%02X:%02X:%02X:%02X:%02X]",
             (uint8_t)(efuseMac), (uint8_t)(efuseMac >> 8), (uint8_t)(efuseMac >> 16),
             (uint8_t)(efuseMac >> 24), (uint8_t)(efuseMac >> 32), (uint8_t)(efuseMac >> 40));
    settings.can_bitrate = 500000;
    Preferences preferences;
    if (preferences.begin("hd_cfg", true)) {                 // read-only
        if (preferences.isKey("ssid")) preferences.getString("ssid", settings.wifi_ssid, sizeof(settings.wifi_ssid));
        preferences.end();
    }
}

void setup() {
    Serial.begin(LOG_BAUD);
    delay(150);
    Serial.printf("\n[SAFEBOOT-FW] %s recovery firmware v%s #%s (factory partition)\n",
                  APP_NAME_STR, FIRMWARE_VERSION_STR, FIRMWARE_BUILD_NUM_STR);

    pinMode(SAFEBOOT_BUTTON_PIN, INPUT_PULLUP);  // GPIO 4: internal pull-up, active LOW (tie to GND)
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
    pinMode(CAN_ACTIVITY_LED_PIN, OUTPUT);       // GPIO 2: repurposed as the "Safe Boot active" indicator
    digitalWrite(CAN_ACTIVITY_LED_PIN, LOW);

    loadSettingsForRecovery();
    SafeBoot::startRecoveryMode();     // brings up Wi-Fi + the recovery HTTP portal on port 80
}

void loop() {
    SafeBoot::handleClient();

    const uint32_t now = millis();

    // CAN LED (GPIO 2): fast 50 ms blink the whole time we are in Safe Boot - a
    // constant "this is Safe Boot" indicator, independent of the Wi-Fi state.
    static uint32_t lastCanToggle = 0;
    if (now - lastCanToggle >= 50) {
        lastCanToggle = now;
        digitalWrite(CAN_ACTIVITY_LED_PIN, !digitalRead(CAN_ACTIVITY_LED_PIN));
    }

    // Status LED (GPIO 15): steady ON when connected as a station; fast blink at a
    // DIFFERENT period (90 ms, asymmetric with the CAN LED's 50 ms) when the device
    // fell back to the rescue Access Point, so the two LEDs are clearly distinguishable.
    if (SafeBoot::isApMode()) {
        static uint32_t lastStatusToggle = 0;
        if (now - lastStatusToggle >= 90) {
            lastStatusToggle = now;
            digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
        }
    } else {
        digitalWrite(STATUS_LED_PIN, HIGH);
    }

    delay(2);
}
