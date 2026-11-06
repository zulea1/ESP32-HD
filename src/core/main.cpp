/**
 * ============================================================================
 * File      : src/core/main.cpp
 * Project   : ESP32 HD - UDS Diagnostic Service Tester (ISO 14229 / ISO 15765-2)
 * Copyright : (c) 2026 Henzulea Daniel email: <zulea1@gmail.com>
 * License   : GNU General Public License v3.0 or later (GPL-3.0-or-later) - see LICENSE
 * Version   : see FIRMWARE_VERSION_STR / FIRMWARE_BUILD_NUM_STR in src/config.h
 * ============================================================================
 * ARCHITECTURAL DESIGN & EXECUTION FLOW:
 * 1. Dedicated Dual-Core FreeRTOS Architecture:
 *    - Core 0 (PRO_CPU): Executes Priority 20 real-time CAN Diagnostic Worker
 *      (`canDiagWorkerTask`). Responsible for non-blocking TWAI FIFO extraction,
 *      UDS frame inspection, ISO-TP multi-frame transport processing, and the
 *      2-second UDS TesterPresent (0x3E 00) keepalive loop to active modules.
 *    - Core 1 (APP_CPU): Executes Priority 5 `systemSupervisorTask` (monitoring
 *      TWAI controller health, TJA1043 fault pin, and GPIO 4 SafeBoot trigger)
 *      and services the HTTP WebServer and OTA flashing routines inside `loop()`
 *      with an expanded 16 KB FreeRTOS `loopTask` stack (`SET_LOOP_TASK_STACK_SIZE(16384)`).
 *
 * 2. Force Safe Boot on GPIO 4:
 *    - At power-up (`setup()`), GPIO 4 is sampled over a debounce window.
 *    - If GPIO 4 is held LOW at power-up (or if NVS `sb_flag` was set via the
 *      Web UI), normal CAN/UDS initialization is bypassed and the device boots
 *      directly into Safe Boot Rescue Mode.
 *    - During normal runtime, holding GPIO 4 LOW for >3 seconds also triggers
 *      an immediate transition into Safe Boot Rescue Mode.
 *
 * 3. Network Provisioning:
 *    - Default STA mode connects to SSID "ESP32-HD" (Password "1122334455").
 *    - Automatically falls back to Open Access Point "ESP-32-HD" (192.168.4.1)
 *      if the station network is unreachable.
 * ============================================================================
 */

#include <Arduino.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "config.h"
#include "safe_boot.h"
#include "can_driver.h"
#include "uds_handler.h"
#include "web_server.h"

// Expand Arduino Core 1 loopTask stack from default 8 KB to 16 KB (16,384 bytes)
// to prevent stack canary panics during concurrent HTTP + WebSocket UDS telemetry serialization
#if defined(SET_LOOP_TASK_STACK_SIZE)
SET_LOOP_TASK_STACK_SIZE(16384);
#endif

TesterSettings    settings;
SystemMetrics     metrics = {};
SemaphoreHandle_t metricsMutex = NULL;

TaskHandle_t canDiagTaskHandle    = NULL;
TaskHandle_t supervisorTaskHandle = NULL;
static bool  inSafeBootMode       = false;

/**
 * @brief Loads user configuration from NVS Flash ("hd_cfg" namespace).
 *        Initializes factory defaults if the partition is unformatted or new.
 */
void loadSettings() {
    Serial.println("[NVS] Initializing Non-Volatile Storage (NVS) partition...");
    esp_err_t nvsInitResult = nvs_flash_init();
    if (nvsInitResult == ESP_ERR_NVS_NO_FREE_PAGES || nvsInitResult == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.println("[NVS] WARNING: NVS partition truncated or version mismatch. Erasing and re-formatting...");
        nvs_flash_deinit();
        nvs_flash_erase();
        nvs_flash_init();
    }

    uint64_t efuseMac = ESP.getEfuseMac();
    char defaultName[48];
    snprintf(
        defaultName,
        sizeof(defaultName),
        "ESP32 HD [%02X:%02X:%02X:%02X:%02X:%02X]",
        (uint8_t)(efuseMac >> 0),
        (uint8_t)(efuseMac >> 8),
        (uint8_t)(efuseMac >> 16),
        (uint8_t)(efuseMac >> 24),
        (uint8_t)(efuseMac >> 32),
        (uint8_t)(efuseMac >> 40)
    );

    Preferences preferences;
    if (!preferences.begin("hd_cfg", false)) {
        Serial.println("[NVS] ERROR: Could not open 'hd_cfg' namespace! Rebuilding NVS...");
        nvs_flash_deinit();
        nvs_flash_erase();
        nvs_flash_init();
        preferences.begin("hd_cfg", false);
    }

    if (!preferences.isKey("nvs_init")) {
        Serial.println("[NVS] First boot detected -> Writing factory default HD tester profile...");
        preferences.clear();
        preferences.putString("node_name", defaultName);
        preferences.putBool("ap_mode", DEFAULT_WIFI_MODE_AP); // false = STA default
        preferences.putString("ssid", DEFAULT_WIFI_SSID);      // "ESP32-HD"
        preferences.putString("pass", DEFAULT_WIFI_PASS);      // "1122334455"
        preferences.putString("ap_ssid", DEFAULT_AP_SSID);    // "ESP-32-HD"
        preferences.putString("ap_pass", DEFAULT_AP_PASS);    // "" (no password)
        preferences.putUInt("bitrate", DEFAULT_CAN_BITRATE);
        // Freshly-initialized NVS (truly first boot, or right after a Safe Boot full factory
        // reset which clears "nvs_init" along with everything else) gets the simplest possible
        // CAN interface assumption: a plain RX/TX transceiver, not TJA1043. See DEFAULT_CAN_CHIP_TYPE.
        preferences.putUChar("can_chip", (uint8_t)DEFAULT_CAN_CHIP_TYPE);
        preferences.putUChar("can_tx_pin", (uint8_t)CAN_TX_PIN);
        preferences.putUChar("can_rx_pin", (uint8_t)CAN_RX_PIN);
        preferences.putUChar("tja_stb_pin", (uint8_t)TJA_STB_PIN);
        preferences.putUChar("tja_en_pin", (uint8_t)TJA_EN_PIN);
        preferences.putUChar("tja_err_pin", (uint8_t)TJA_ERR_PIN);
        preferences.putUChar("tja_led_pin", (uint8_t)CAN_ACTIVITY_LED_PIN);
        preferences.putBool("tja_slp_en", DEFAULT_TJA_SLEEP_ENABLE);
        preferences.putUInt("tja_slp_sec", DEFAULT_TJA_SLEEP_TIMEOUT_SEC);
        preferences.putBool("sb_flag", false);
        preferences.putBool("nvs_init", true);
        Serial.println("[NVS] Factory defaults committed to NVS ('hd_cfg').");
    }

    String nodeName = preferences.getString("node_name", defaultName);
    strncpy(settings.node_name, nodeName.c_str(), sizeof(settings.node_name) - 1);
    settings.node_name[sizeof(settings.node_name) - 1] = '\0';

    settings.wifi_ap_mode = preferences.getBool("ap_mode", DEFAULT_WIFI_MODE_AP);
    String staSsid = preferences.getString("ssid", DEFAULT_WIFI_SSID);
    String staPass = preferences.getString("pass", DEFAULT_WIFI_PASS);
    String apSsid  = preferences.getString("ap_ssid", DEFAULT_AP_SSID);
    String apPass  = preferences.getString("ap_pass", DEFAULT_AP_PASS);

    strncpy(settings.wifi_ssid, staSsid.c_str(), sizeof(settings.wifi_ssid) - 1);
    settings.wifi_ssid[sizeof(settings.wifi_ssid) - 1] = '\0';
    strncpy(settings.wifi_pass, staPass.c_str(), sizeof(settings.wifi_pass) - 1);
    settings.wifi_pass[sizeof(settings.wifi_pass) - 1] = '\0';

    strncpy(settings.ap_ssid, apSsid.c_str(), sizeof(settings.ap_ssid) - 1);
    settings.ap_ssid[sizeof(settings.ap_ssid) - 1] = '\0';
    strncpy(settings.ap_pass, apPass.c_str(), sizeof(settings.ap_pass) - 1);
    settings.ap_pass[sizeof(settings.ap_pass) - 1] = '\0';

    settings.can_bitrate = preferences.getUInt("bitrate", DEFAULT_CAN_BITRATE);

    // CAN chip type: fallback here is CAN_CHIP_TJA1043 (not DEFAULT_CAN_CHIP_TYPE / PLAIN).
    // This read runs on every boot, including a firmware upgrade on an already-deployed board
    // that set "nvs_init" long before this feature existed and therefore never ran the
    // defaults-writing block above. Such a board is physically wired for TJA1043 today, so its
    // behavior must not silently change just because it updated firmware. A truly fresh flash or
    // a post-factory-reset boot DOES run the defaults-writing block above, which explicitly writes
    // "can_chip" = DEFAULT_CAN_CHIP_TYPE (PLAIN) - that explicit write is what makes factory reset
    // land on the simple RX/TX interface, not this fallback value.
    settings.can_chip_type = preferences.getUChar("can_chip", (uint8_t)CAN_CHIP_TJA1043);

    settings.can_tx_pin  = preferences.getUChar("can_tx_pin", (uint8_t)CAN_TX_PIN);
    settings.can_rx_pin  = preferences.getUChar("can_rx_pin", (uint8_t)CAN_RX_PIN);
    settings.tja_stb_pin = preferences.getUChar("tja_stb_pin", (uint8_t)TJA_STB_PIN);
    settings.tja_en_pin  = preferences.getUChar("tja_en_pin", (uint8_t)TJA_EN_PIN);
    settings.tja_err_pin = preferences.getUChar("tja_err_pin", (uint8_t)TJA_ERR_PIN);
    settings.tja_led_pin = (uint8_t)CAN_ACTIVITY_LED_PIN;  // hardwired to GPIO 2 - not configurable

    settings.tja_sleep_enable      = preferences.getBool("tja_slp_en", DEFAULT_TJA_SLEEP_ENABLE);
    settings.tja_sleep_timeout_sec = preferences.getUInt("tja_slp_sec", DEFAULT_TJA_SLEEP_TIMEOUT_SEC);
    settings.safeboot_requested    = preferences.getBool("sb_flag", false);
    settings.canlog_enabled        = preferences.getBool("canlog_en", false);   // default OFF: no CAN/UDS serial flood
    settings.devtest_enabled       = preferences.getBool("devtest_en", false);  // default OFF: real brake-pressure gate applies
    settings.active_module_id      = 0;
    settings.active_module_name[0] = '\0';

    preferences.end();

    Serial.printf("[NVS] Loaded Profile: Node='%s' | PrefWiFi=%s | STA_SSID='%s' | AP_SSID='%s' | CAN=%u kbps | ChipType=%s | SafeBootFlag=%s\n",
                  settings.node_name,
                  settings.wifi_ap_mode ? "AP" : "STA",
                  settings.wifi_ssid,
                  settings.ap_ssid,
                  settings.can_bitrate / 1000U,
                  (settings.can_chip_type == CAN_CHIP_TJA1043) ? "TJA1043" : "PLAIN",
                  settings.safeboot_requested ? "TRUE" : "FALSE");
}

/**
 * @brief Initializes Wi-Fi networking.
 *        Attempts STA connection first (if configured for STA mode). If STA
 *        cannot connect within 8 seconds, automatically falls back to Open AP.
 */
void initWiFiNetwork() {
    Serial.println("[WIFI] Resetting Wi-Fi radio and clearing transient states...");
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    delay(100);

    bool staSuccess = false;

    // 1. If configured for STA mode (default), attempt connection
    if (!settings.wifi_ap_mode) {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.setSleep(false);
        Serial.printf("[WIFI] Attempting STA connection to SSID: '%s' (timeout: 8000 ms)\n", settings.wifi_ssid);
        WiFi.begin(settings.wifi_ssid, settings.wifi_pass);

        uint32_t startAttempt = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt < 8000)) {
            delay(250);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            staSuccess = true;
            settings.wifi_ap_mode = false;
            Serial.printf("[WIFI] STA Connection Established! IP=%s | Gateway=%s | RSSI=%d dBm | MAC=%s\n",
                          WiFi.localIP().toString().c_str(),
                          WiFi.gatewayIP().toString().c_str(),
                          WiFi.RSSI(),
                          WiFi.macAddress().c_str());
        } else {
            Serial.printf("[WIFI] WARNING: Could not connect to STA SSID '%s' within 8s (status=%d).\n",
                          settings.wifi_ssid, (int)WiFi.status());
            Serial.println("[WIFI] Switching to AP Mode (SSID/password from saved settings)...");
        }
    }

    // 2. Fallback to Open AP mode if STA connection was not successful or if explicitly configured
    if (!staSuccess) {
        settings.wifi_ap_mode = true;
        WiFi.disconnect(true, true);
        delay(100);
        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(
            IPAddress(192, 168, 4, 1),
            IPAddress(192, 168, 4, 1),
            IPAddress(255, 255, 255, 0)
        );
        // AP name/password come from NVS (settings). WPA2 needs >= 8 chars; shorter/empty -> open AP.
        const char *apName = (settings.ap_ssid[0] != '\0') ? settings.ap_ssid : DEFAULT_AP_SSID;
        const bool  apSecured = (strlen(settings.ap_pass) >= 8);
        WiFi.softAP(apName, apSecured ? settings.ap_pass : NULL);
        WiFi.setSleep(false);
        Serial.printf("[WIFI] AP Active! SSID='%s' | Security=%s | Portal=http://%s/ | AP_MAC=%s\n",
                      apName,
                      apSecured ? "WPA2" : "OPEN",
                      WiFi.softAPIP().toString().c_str(),
                      WiFi.softAPmacAddress().c_str());
    }
}

/**
 * ============================================================================
 * CORE 0 TASK: High-Speed Real-Time CAN Diagnostic Engine
 * ============================================================================
 * Pinned to Core 0 (PRO_CPU) at FreeRTOS Priority 20.
 * Continuously drains the ESP32 TWAI 128-frame RX FIFO in non-blocking bursts,
 * passes each frame into UdsHandler::inspectFrame() for ISO-TP multi-frame
 * reassembly and Flow Control (0x30) transmission, and services background
 * TesterPresent (0x3E 00) keepalives.
 */
void canDiagWorkerTask(void *pvParameters) {
    (void)pvParameters;
    Serial.printf("[RTOS] canDiagWorkerTask started on Core %d (Priority 20, Stack %u B)\n",
                  xPortGetCoreID(), TASK_STACK_DIAG);

    static twai_message_t receivedFrameBurst[32];

    for (;;) {
        // Block up to 1 ms on the TWAI RX queue: wakes immediately on a frame (lower latency than poll+delay)
        // and yields the core while idle. If the driver is down (returns instantly) the guard below sleeps.
        const int64_t receiveWaitStartMicros = esp_timer_get_time();
        size_t receivedFrameCount = CanDriver::receiveBurst(receivedFrameBurst, 32, 1);

        if (receivedFrameCount > 0) {
            if (xSemaphoreTake(metricsMutex, 0) == pdTRUE) {
                metrics.can_rx_count += receivedFrameCount;
                xSemaphoreGive(metricsMutex);
            }
            for (size_t frameIndex = 0; frameIndex < receivedFrameCount; frameIndex++) {
                UdsHandler::inspectFrame(receivedFrameBurst[frameIndex]);
            }
        }

        UdsHandler::loop();
        CanDriver::serviceTransceiverPowerAndLed();

        if (receivedFrameCount == 0 && (esp_timer_get_time() - receiveWaitStartMicros) < 400) {
            vTaskDelay(1);   // receive returned instantly (driver stopped/uninstalled) -> avoid starving IDLE/WDT
        }
    }
}

/**
 * ============================================================================
 * CORE 1 TASK: System Supervisor, TWAI Health & GPIO 4 SafeBoot Monitor
 * ============================================================================
 * Pinned to Core 1 (APP_CPU) at FreeRTOS Priority 5.
 * Polls TWAI controller error counters and Bus-Off states every 100ms, drives
 * the GPIO 15 session status LED, and monitors GPIO 4 for a 3-second LOW hold
 * to trigger Safe Boot Rescue Mode at runtime.
 */

// ---------------------------------------------------------------------------
// Runtime health monitor (called from systemSupervisorTask every HEALTH_CHECK_MS).
// Added to hunt an intermittent "LoadProhibited" panic seen inside core 1's IDLE task. That pattern
// (idle task + IRAM + garbage pointer) usually means the heap was corrupted earlier by an out-of-bounds
// write and only blew up when the damaged block was freed. This check walks the whole heap and, if it is
// damaged, prints the first bad block immediately - long before the crash - and logs per-task stack
// headroom so a task that is running out of stack shows up too.
// ---------------------------------------------------------------------------
static const uint32_t HEALTH_CHECK_MS        = 5000;
static const uint32_t STACK_WARN_BYTES       = 1024;   // warn threshold for our own tasks
// The two FreeRTOS IDLE tasks have a small fixed stack (CONFIG_FREERTOS_IDLE_TASK_STACKSIZE, ~1536 B) and
// normally sit around 450-600 B free - that is healthy, they only feed the task watchdog and reap deleted
// tasks. Use a much lower bar for them so they do not warn every cycle, while still catching a real squeeze.
static const uint32_t STACK_WARN_BYTES_IDLE  = 192;
static const uint32_t HEALTH_REPORT_EVERY    = 12;     // full report every 12 checks (~1 min)

static void logTaskStack(const char *taskName, TaskHandle_t taskHandle, bool forceReport, uint32_t warnBytes = STACK_WARN_BYTES) {
    if (taskHandle == nullptr) return;
    const uint32_t freeBytes = (uint32_t)uxTaskGetStackHighWaterMark(taskHandle);   // bytes on ESP-IDF
    const bool low = freeBytes < warnBytes;
    if (forceReport || low)
        Serial.printf("[HEALTH] stack %-12s min free %5u B%s\n", taskName, (unsigned)freeBytes, low ? "  <-- LOW" : "");
}

static void runHealthCheck() {
    static uint32_t checkCount = 0;
    static bool heapCorruptionReported = false;
    const bool fullReport = (checkCount++ % HEALTH_REPORT_EVERY) == 0;

    if (!heap_caps_check_integrity_all(true) && !heapCorruptionReported) {   // true = print details of the bad block
        heapCorruptionReported = true;
        Serial.println("[HEALTH] *** HEAP CORRUPTION DETECTED *** (details above) - note what you clicked just before this line");
    }
    const uint32_t freeHeap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t minFreeHeap = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    const uint32_t largestBlock = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (fullReport || minFreeHeap < 20000)
        Serial.printf("[HEALTH] heap free %u B, min ever %u B, largest block %u B\n", (unsigned)freeHeap, (unsigned)minFreeHeap, (unsigned)largestBlock);

    logTaskStack("loopTask",     xTaskGetHandle("loopTask"),     fullReport);
    logTaskStack("HD_CAN_Diag",  canDiagTaskHandle,              fullReport);
    logTaskStack("HD_Sys_Super", supervisorTaskHandle,           fullReport);
    logTaskStack("udsPoll",      xTaskGetHandle("udsPoll"),      fullReport);
    logTaskStack("logTask",      xTaskGetHandle("logTask"),      fullReport);
    logTaskStack("IDLE1",        xTaskGetIdleTaskHandleForCore(1), fullReport, STACK_WARN_BYTES_IDLE);
    logTaskStack("IDLE0",        xTaskGetIdleTaskHandleForCore(0), fullReport, STACK_WARN_BYTES_IDLE);
}

void systemSupervisorTask(void *pvParameters) {
    (void)pvParameters;
    Serial.printf("[RTOS] systemSupervisorTask started on Core %d (Priority 5, Stack %u B)\n",
                  xPortGetCoreID(), TASK_STACK_SUPERVISOR);

    uint32_t buttonHoldStart = 0;
    bool blinkState = false;

    for (;;) {
        CanDriver::checkAlertsAndRecover();

        if (UdsHandler::isTesterPresentEnabled()) {
            digitalWrite(STATUS_LED_PIN, HIGH);
        } else {
            blinkState = !blinkState;
            digitalWrite(STATUS_LED_PIN, blinkState ? HIGH : LOW);
        }

        // Monitor GPIO 4 during runtime: holding LOW > 3 seconds forces SafeBoot
        if (digitalRead(SAFEBOOT_BUTTON_PIN) == LOW) {
            if (buttonHoldStart == 0) {
                buttonHoldStart = millis();
                Serial.println("[SAFEBOOT] GPIO 4 detected LOW during runtime. Hold for 3s to enter SafeBoot Rescue Mode...");
            } else if (millis() - buttonHoldStart > 3000) {
                Serial.println("[SAFEBOOT] GPIO 4 held LOW >3s! Setting SafeBoot flag and rebooting into Rescue Portal...");
                SafeBoot::enableAndReboot();
            }
        } else {
            if (buttonHoldStart != 0) {
                Serial.println("[SAFEBOOT] GPIO 4 released before 3s threshold. Continuing normal operation.");
            }
            buttonHoldStart = 0;
        }

        static uint32_t lastHealthCheckMs = 0;
        if (millis() - lastHealthCheckMs >= HEALTH_CHECK_MS) { lastHealthCheckMs = millis(); runHealthCheck(); }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * @brief Checks if GPIO 4 (SAFEBOOT_BUTTON_PIN) is kept LOW when powering up.
 *        Samples GPIO 4 across a 250ms power-on stabilization window so that
 *        keeping GPIO 4 LOW at power-up reliably forces Safe Boot mode.
 */
static bool checkPowerUpGpio4ForceSafeBoot() {
    if (digitalRead(SAFEBOOT_BUTTON_PIN) != LOW) {
        Serial.println("[BOOT] GPIO 4 is HIGH (Normal Mode). Proceeding with standard boot sequence.");
        return false;
    }

    Serial.println("[BOOT] NOTICE: GPIO 4 detected LOW at power-up! Verifying continuous LOW state (250 ms)...");
    uint32_t debounceWindowStartMillis = millis();
    while (millis() - debounceWindowStartMillis < 250) {
        if (digitalRead(SAFEBOOT_BUTTON_PIN) != LOW) {
            Serial.println("[BOOT] GPIO 4 returned HIGH during debounce window. Continuing normal boot.");
            return false;
        }
        delay(25);
    }

    Serial.println("[BOOT] CONFIRMED: GPIO 4 kept LOW at power-up -> Forcing Safe Boot Rescue Mode!");
    return true;
}

void setup() {
    Serial.begin(LOG_BAUD);   // gAsyncLog: starts the UART + the drain task
    delay(150);
    Serial.println("\n======================================================================");
    Serial.printf(" %s | Firmware v%s (Build #%s) | %s\n",
                  APP_NAME_STR, FIRMWARE_VERSION_STR, FIRMWARE_BUILD_NUM_STR, APP_COPYRIGHT_STR);
    Serial.println(" Dedicated HD UDS Diagnostic Service Tester (ISO 14229)");
    Serial.printf(" MCU: %s Rev %d @ %u MHz | Flash: %u MB | Free Heap: %u B\n",
                  ESP.getChipModel(),
                  ESP.getChipRevision(),
                  ESP.getCpuFreqMHz(),
                  ESP.getFlashChipSize() / (1024 * 1024),
                  ESP.getFreeHeap());
    Serial.printf(" Pinout: CAN_TX=GPIO%d, CAN_RX=GPIO%d, STB=GPIO%d, EN=GPIO%d, ERR=GPIO%d, SAFEBOOT=GPIO%d\n",
                  CAN_TX_PIN, CAN_RX_PIN, TJA_STB_PIN, TJA_EN_PIN, TJA_ERR_PIN, SAFEBOOT_BUTTON_PIN);
    Serial.println("======================================================================");

    // Which partition are we running from, and confirm a freshly-OTA'd image so the bootloader keeps it
    // (a "pending verify" app would otherwise roll back to the factory/safe-boot partition on the next reset).
    const esp_partition_t *runPart = esp_ota_get_running_partition();
    Serial.printf("[BOOT] Running from partition '%s' @0x%06X (subtype %d)\n",
                  runPart ? runPart->label : "?", runPart ? (unsigned)runPart->address : 0, runPart ? runPart->subtype : -1);
    esp_ota_img_states_t otaState;
    if (runPart && esp_ota_get_state_partition(runPart, &otaState) == ESP_OK && otaState == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
            Serial.println("[BOOT] Freshly-updated app confirmed valid (rollback cancelled).");
    }

    // GPIO 4 is a general-purpose pin; enable the internal pull-up so it idles HIGH
    // and reads LOW only when tied to GND (no external pull-up resistor required).
    pinMode(SAFEBOOT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);

    metricsMutex = xSemaphoreCreateMutex();
    loadSettings();

    // Check if Safe Boot was requested via NVS flag OR forced via GPIO 4 held LOW at power-up
    bool gpio4Forced = checkPowerUpGpio4ForceSafeBoot();
    if (settings.safeboot_requested || gpio4Forced) {
        Serial.printf("[BOOT] Safe Boot requested (%s) - handing off to the FACTORY recovery firmware\n",
                      gpio4Forced ? "GPIO 4 held LOW at power-up" : "Web UI / NVS flag");
        // This (main app, ota_0) no longer runs recovery in-binary: erase otadata so the bootloader runs the
        // separate minimal safe-boot firmware in the factory partition, then update/recover from there.
        SafeBoot::enableAndReboot();   // erases otadata + restarts (unreached afterwards)
        return;
    }

    initWiFiNetwork();
    CanDriver::init(settings.can_bitrate);
    UdsHandler::setTesterPresent(false, 0);
    MainWebServer::init();

    xTaskCreatePinnedToCore(
        canDiagWorkerTask,
        "HD_CAN_Diag",
        TASK_STACK_DIAG,
        NULL,
        20,
        &canDiagTaskHandle,
        CORE_CAN_DIAG_WORKER
    );

    xTaskCreatePinnedToCore(
        systemSupervisorTask,
        "HD_Sys_Super",
        TASK_STACK_SUPERVISOR,
        NULL,
        5,
        &supervisorTaskHandle,
        CORE_SYSTEM_WEB_OTA
    );

    Serial.println("[BOOT] System initialization complete. Ready for HD UDS diagnostics.");
}

void loop() {
    if (inSafeBootMode) {
        SafeBoot::handleClient();
    } else {
        MainWebServer::handleClient();
    }
    vTaskDelay(pdMS_TO_TICKS(2));
}
