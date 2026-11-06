/**
 * ============================================================================
 * File     : src/core/safe_boot.cpp
 * Project  : ESP32 HD - UDS Diagnostic Service Tester (ISO 14229 / ISO 15765-2)
 * Copyright: (c) 2026 Henzulea Daniel email: <zulea1@gmail.com>
 * License  : GNU General Public License v3.0 or later (GPL-3.0-or-later) - see LICENSE
 * Version   : see FIRMWARE_VERSION_STR / FIRMWARE_BUILD_NUM_STR in src/config.h
 * ============================================================================
 * ARCHITECTURAL OVERVIEW:
 *   Implements the isolated Safe Boot Rescue Mode and OTA Recovery Portal.
 *   - Hardware Trigger: Force Safe Boot on GPIO 4 (`GPIO_NUM_4`). If GPIO 4
 *     is kept LOW during power-up (or held LOW for 3s at runtime), `main.cpp`
 *     routes execution directly to `SafeBoot::startRecoveryMode()`.
 *   - CAN / TWAI Isolation: The TWAI peripheral and TJA1043 transceiver are
 *     left uninitialized in Safe Boot so bus interrupts cannot disrupt flash
 *     writes during emergency `.bin` OTA updates.
 *   - Dual-Stage Network Bring-Up:
 *     Stage 1 attempts to connect to the stored Station Wi-Fi (`ESP32-HD`).
 *     Stage 2 falls back to an open rescue Access Point (`ESP-32-HD` at
 *     `192.168.4.1`) if STA fails within 8 seconds.
 * ============================================================================
 */

#include "safe_boot.h"
#include "config.h"
#include "web_ui/safeboot_html.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <Update.h>
#include "ota_helper.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"

static WebServer safeServer(80);
static String    activeSafeSsid   = SAFEBOOT_AP_SSID;
static bool      safeBootInApMode = false;

void SafeBoot::enableAndReboot() {
    // Tasmota-style handoff: erase the otadata partition so the 2nd-stage bootloader falls back to the FACTORY
    // (safe boot) firmware on the next restart. The main app lives in ota_0; with otadata cleared the bootloader
    // no longer selects it and runs factory instead. (Also clears the legacy sb_flag so the app won't loop.)
    Serial.println("[SAFEBOOT] Erasing otadata -> next boot runs the factory (safe boot) firmware. Restarting...");
    Preferences preferences;
    if (preferences.begin("hd_cfg", false)) { preferences.putBool("sb_flag", false); preferences.end(); }
    const esp_partition_t *otadata = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (otadata) esp_partition_erase_range(otadata, 0, otadata->size);
    else Serial.println("[SAFEBOOT] WARNING: otadata partition not found - cannot force factory boot.");
    delay(150);
    ESP.restart();
}

String SafeBoot::getApSsid() {
    return activeSafeSsid;
}

bool SafeBoot::isApMode() {
    return safeBootInApMode;
}

void SafeBoot::handleClient() {
    safeServer.handleClient();
}

void SafeBoot::startRecoveryMode() {
    Serial.println("--------------------------------------------------------------");
    Serial.printf("[SAFEBOOT] Initializing Safe Boot Recovery Mode | %s (safe boot) v%s #%s\n",
                  APP_NAME_STR, SAFEBOOT_VERSION_STR, SAFEBOOT_BUILD_NUM_STR);
    Serial.printf("[SAFEBOOT] Author: %s | Hardware Trigger Pin: GPIO %d\n",
                  APP_COPYRIGHT_STR, (int)SAFEBOOT_BUTTON_PIN);
    Serial.println("--------------------------------------------------------------");

    // Partition diagnostics: where we are running from, and where a firmware upload will be written.
    const esp_partition_t *runPart   = esp_ota_get_running_partition();
    const esp_partition_t *otaTarget = esp_ota_get_next_update_partition(NULL);
    Serial.printf("[SAFEBOOT] running partition: '%s' @0x%06X subtype=%d size=%uKB\n",
                  runPart ? runPart->label : "?", runPart ? (unsigned)runPart->address : 0,
                  runPart ? runPart->subtype : -1, runPart ? (unsigned)(runPart->size / 1024) : 0);
    Serial.printf("[SAFEBOOT] OTA target slot : %s @0x%06X size=%uKB  <- a firmware upload writes here\n",
                  otaTarget ? otaTarget->label : "(NONE - OTA cannot run!)",
                  otaTarget ? (unsigned)otaTarget->address : 0,
                  otaTarget ? (unsigned)(otaTarget->size / 1024) : 0);
    // This recovery fw MUST live in the factory partition (0x10000). If it was flashed into an OTA slot instead
    // (e.g. a plain `pio upload`), there is no free slot to receive a firmware upload - warn loudly.
    if (runPart && runPart->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        Serial.println("[SAFEBOOT] *** WARNING: NOT running from the factory partition! ***");
        Serial.println("[SAFEBOOT] *** This safe-boot fw was flashed into an OTA slot, so a firmware upload has ***");
        Serial.println("[SAFEBOOT] *** nowhere to go. Flash it to factory:  esptool write_flash 0x10000 <safeboot firmware.bin> ***");
    }
    Serial.println("--------------------------------------------------------------");

    Preferences preferences;
    preferences.begin("hd_cfg", false);
    preferences.putBool("sb_flag", false);
    String storedStaSsid = preferences.isKey("ssid") ? preferences.getString("ssid", "") : "";
    String storedStaPass = preferences.isKey("pass") ? preferences.getString("pass", "") : "";
    preferences.end();

    storedStaSsid.trim();
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);              // initialise the Wi-Fi driver BEFORE any disconnect (avoids ESP_ERR_WIFI_NOT_INIT)
    WiFi.setSleep(false);
    WiFi.disconnect(true, true);
    delay(100);

    // Attempt one STA connection for up to 8 s (blinking the LED). Returns true on success.
    auto tryStation = [](const char *ssid, const char *pass, const char *label) -> bool {
        if (!ssid || !ssid[0]) return false;
        Serial.printf("[SAFEBOOT] STA attempt [%s]: SSID '%s'\n", label, ssid);
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, pass);
        uint32_t startMs = millis();
        uint32_t lastCanToggle = 0, lastStatusToggle = 0;
        while (WiFi.status() != WL_CONNECTED && (millis() - startMs < 8000)) {
            uint32_t now = millis();
            // CAN LED (GPIO 2): fast blink the whole time we are in Safe Boot.
            if (now - lastCanToggle >= 50) {
                lastCanToggle = now;
                digitalWrite(CAN_ACTIVITY_LED_PIN, !digitalRead(CAN_ACTIVITY_LED_PIN));
            }
            // Status LED (GPIO 15): slow blink while attempting the STA connection.
            if (now - lastStatusToggle >= 250) {
                lastStatusToggle = now;
                digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
                Serial.print(".");
            }
            delay(10);
        }
        Serial.println();
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[SAFEBOOT] Connected [%s]! IP: %s  RSSI: %d dBm\n",
                          label, WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
            return true;
        }
        Serial.printf("[SAFEBOOT] STA attempt [%s] timed out after 8 s.\n", label);
        return false;
    };

    bool staConnected = false;

    // Stage 1: STA using the NVS-stored credentials.
    if (tryStation(storedStaSsid.c_str(), storedStaPass.c_str(), "NVS")) {
        staConnected = true; safeBootInApMode = false; activeSafeSsid = storedStaSsid;
    }
    // Stage 2: STA using the hardcoded default credentials (SAFEBOOT_STA_FALLBACK_*).
    if (!staConnected && tryStation(SAFEBOOT_STA_FALLBACK_SSID, SAFEBOOT_STA_FALLBACK_PASS, "default")) {
        staConnected = true; safeBootInApMode = false; activeSafeSsid = String(SAFEBOOT_STA_FALLBACK_SSID);
    }
    // Stage 3: WPA2 rescue Access Point.
    if (!staConnected) {
        safeBootInApMode = true;
        activeSafeSsid   = String(SAFEBOOT_AP_SSID);
        Serial.printf("[SAFEBOOT] Stage 3: Launching rescue AP '%s' (WPA2, pass '%s') at 192.168.4.1\n",
                      activeSafeSsid.c_str(), SAFEBOOT_AP_PASS);
        WiFi.disconnect(true, true);
        delay(100);
        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
        WiFi.softAP(activeSafeSsid.c_str(), SAFEBOOT_AP_PASS);
        WiFi.setSleep(false);
    }

    digitalWrite(STATUS_LED_PIN, HIGH);
    String portalIp = safeBootInApMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

    Serial.println("==============================================================");
    Serial.printf("[SAFEBOOT] ESP32 HD Recovery Portal Active at http://%s/\n", portalIp.c_str());
    Serial.println("==============================================================");

    safeServer.on("/", HTTP_GET, []() {
        Serial.println("[SAFEBOOT] GET / -> Serving SAFEBOOT_HTML recovery portal");
        safeServer.sendHeader("Content-Encoding", "gzip");
        safeServer.sendHeader("Cache-Control", "no-cache");
        safeServer.send_P(200, "text/html", (PGM_P)SAFEBOOT_HTML_GZ, SAFEBOOT_HTML_GZ_LEN);
    });

    safeServer.on("/api/safeboot/info", HTTP_GET, []() {
        String clientIpAddress = SafeBoot::isApMode() ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
        int signalStrengthRssi  = SafeBoot::isApMode() ? 0 : (int)WiFi.RSSI();

        char jsonResponseBuffer[680];
        snprintf(
            jsonResponseBuffer,
            sizeof(jsonResponseBuffer),
            "{\"node_name\":\"%s\",\"copyright\":\"%s\",\"safeboot_gpio\":%d,\"chip\":\"%s\",\"rev\":%d,\"cpu_mhz\":%u,\"uptime\":%lu,"
            "\"flash\":%u,\"sketch_size\":%u,\"ota_slot_size\":%u,\"heap\":%u,\"mac\":\"%s\","
            "\"ap_mode\":%s,\"wifi_mode\":\"%s\",\"ssid\":\"%s\",\"nvs_ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
            "\"can_bitrate\":%u,\"fw_version\":\"%s\",\"fw_build\":\"%s\"}",
            settings.node_name,
            APP_COPYRIGHT_STR,
            (int)SAFEBOOT_BUTTON_PIN,
            ESP.getChipModel(),
            ESP.getChipRevision(),
            ESP.getCpuFreqMHz(),
            millis() / 1000UL,
            ESP.getFlashChipSize(),
            ESP.getSketchSize(),
            OTA_PARTITION_SIZE_BYTES,
            ESP.getFreeHeap(),
            WiFi.macAddress().c_str(),
            SafeBoot::isApMode() ? "true" : "false",
            SafeBoot::isApMode() ? "Rescue AP (WPA2)" : "Station (STA)",
            SafeBoot::getApSsid().c_str(),
            settings.wifi_ssid,
            clientIpAddress.c_str(),
            signalStrengthRssi,
            settings.can_bitrate / 1000U,
            SAFEBOOT_VERSION_STR,
            SAFEBOOT_BUILD_NUM_STR
        );
        safeServer.send(200, "application/json", jsonResponseBuffer);
    });

    auto handleFactoryReset = []() {
        Serial.println("[SAFEBOOT] Factory Resetting HD NVS settings and erasing flash partition...");
        Preferences preferences;
        if (preferences.begin("hd_cfg", false)) {
            preferences.clear();
            preferences.end();
        }
        nvs_flash_deinit();
        nvs_flash_erase();
        nvs_flash_init();
        safeServer.send(200, "application/json", "{\"ok\":true}");
        delay(350);
        ESP.restart();
    };
    safeServer.on("/api/safeboot/factory_reset", HTTP_POST, handleFactoryReset);
    safeServer.on("/reset", HTTP_POST, handleFactoryReset);

    auto handleNormalReboot = []() {
        // Point the bootloader back at the main app (ota_0) and restart. Only succeeds if ota_0 holds a valid
        // app image; otherwise the bootloader falls straight back here to safe boot.
        Serial.println("[SAFEBOOT] Setting ota_0 bootable and rebooting into the main HD app...");
        Preferences preferences;
        if (preferences.begin("hd_cfg", false)) { preferences.putBool("sb_flag", false); preferences.end(); }
        const esp_partition_t *app0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        esp_err_t setResult = ESP_FAIL;
        if (app0) setResult = esp_ota_set_boot_partition(app0);
        safeServer.send(200, "application/json",
                        setResult == ESP_OK ? "{\"ok\":true}"
                                            : "{\"ok\":false,\"error\":\"no valid app in ota_0 - upload firmware first\"}");
        if (setResult == ESP_OK) { delay(250); ESP.restart(); }
    };
    safeServer.on("/api/safeboot/reboot", HTTP_POST, handleNormalReboot);
    safeServer.on("/reboot", HTTP_POST, handleNormalReboot);

    safeServer.on(
        "/update",
        HTTP_POST,
        []() {
            bool ok = OtaHelper::respond(safeServer);
            if (ok) {
                // Clear the safe-boot request flag so the freshly flashed app boots NORMALLY instead of coming
                // straight back here, then point the bootloader at ota_0 (where the OTA just wrote the app).
                Preferences preferences;
                if (preferences.begin("hd_cfg", false)) { preferences.putBool("sb_flag", false); preferences.end(); }
                const esp_partition_t *app0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
                esp_err_t setResult = app0 ? esp_ota_set_boot_partition(app0) : ESP_FAIL;
                Serial.printf("[SAFEBOOT] OTA complete -> cleared sb_flag, set boot partition to ota_0: %s. Restarting into the app...\n",
                              setResult == ESP_OK ? "OK" : "FAILED (app image may be invalid)");
                delay(800);
                ESP.restart();
            } else {
                Serial.println("[SAFEBOOT] OTA upload failed - staying in safe boot (see error above).");
            }
        },
        []() { OtaHelper::feed(safeServer.upload(), "SAFEBOOT-OTA"); }
    );

    safeServer.begin();
}
