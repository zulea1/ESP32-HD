/**
 * ============================================================================
 * File     : src/core/safe_boot.h
 * Project  : ESP32 HD - UDS Diagnostic Service Tester (ISO 14229 / ISO 15765-2)
 * Copyright: (c) 2026 Henzulea Daniel email: <zulea1@gmail.com>
 * License  : GNU General Public License v3.0 or later (GPL-3.0-or-later) - see LICENSE
 * Version   : see FIRMWARE_VERSION_STR / FIRMWARE_BUILD_NUM_STR in src/config.h
 * ============================================================================
 * DESCRIPTION:
 *   Safe Boot & OTA Partition Recovery Manager interface.
 *   Entered when:
 *     1. GPIO 4 (SAFEBOOT_BUTTON_PIN) is kept LOW when powered up,
 *     2. GPIO 4 is held LOW for 3 seconds during normal operation, or
 *     3. The user clicks "Safe Boot" inside the Web UI Settings modal.
 *   Suspends all CAN bus (TWAI) and UDS tasks to guarantee safe OTA partition
 *   flashing (`app0` / `app1`) and NVS factory reset recovery.
 * ============================================================================
 */
#pragma once
#include <Arduino.h>

class SafeBoot {
public:
    /**
     * @brief Sets the persistent NVS `sb_flag` and restarts the ESP32 into Safe Boot.
     */
    static void enableAndReboot();

    /**
     * @brief Starts the Safe Boot Wi-Fi recovery network and HTTP server on port 80.
     */
    static void startRecoveryMode();

    /**
     * @brief Services incoming HTTP recovery requests in the main loop.
     */
    static void handleClient();

    /**
     * @brief Returns the active SSID used by Safe Boot mode.
     */
    static String getApSsid();

    /**
     * @brief Returns true if Safe Boot is operating in Open Rescue AP mode.
     */
    static bool isApMode();
};
