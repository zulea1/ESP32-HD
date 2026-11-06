/**
 * ============================================================================
 * File      : src/core/can_driver.h
 * Project   : ESP32 HD - UDS Diagnostic Service Tester (ISO 14229 / ISO 15765-2)
 * Copyright : (c) 2026 Henzulea Daniel email: <zulea1@gmail.com>
 * License   : GNU General Public License v3.0 or later (GPL-3.0-or-later) - see LICENSE
 * Version   : see FIRMWARE_VERSION_STR / FIRMWARE_BUILD_NUM_STR in src/config.h
 * ============================================================================
 * OVERVIEW:
 * Thread-safe hardware abstraction layer for the ESP32 internal TWAI (SJA1000)
 * CAN 2.0A/2.0B controller and the external CAN transceiver, which can be either
 * a TJA1043 high-speed CAN transceiver (STB/EN standby control +
 * ERR_N fault sensing) or a plain high-speed transceiver wired with only TX/RX
 * (no control lines), selected at runtime via settings.can_chip_type.
 * ============================================================================
 */
#pragma once
#include <Arduino.h>
#include <driver/twai.h>

class CanDriver {
public:
    /**
     * @brief Initializes the active CAN transceiver (TJA1043 control pins, or a plain
     *        RX/TX transceiver with no control lines, per settings.can_chip_type), creates
     *        the cross-core CAN TX mutex, and starts the ESP32 TWAI (SJA1000) controller at
     *        the specified bitrate with the UDS (0x7C0-0x7FF) hardware filter enabled by
     *        default after reboot.
     */
    static bool init(uint32_t canBusBitrate);

    /**
     * @brief Thread-safely transmits a single CAN 2.0A/2.0B frame onto the
     *        physical CAN bus. Protects against cross-core collisions via
     *        FreeRTOS mutex, wakes the TJA1043 transceiver if sleeping, and
     *        auto-recovers the TWAI controller if stopped or in Bus-Off state.
     */
    static bool transmit(uint32_t canIdentifier, bool isExtendedFrame, bool isRemoteFrame, uint8_t dataLengthCode, const uint8_t *frameDataBytes);

    /**
     * @brief Drains up to `maximumFrameCount` immediately available in the TWAI
     *        hardware RX queue without blocking (`timeoutMilliseconds = 0`).
     */
    static size_t receiveBurst(twai_message_t *receivedFrameBuffer, size_t maximumFrameCount, uint32_t timeoutMilliseconds = 0);

    /**
     * @brief Dynamically reconfigures the SJA1000 silicon acceptance filter
     *        (Stop -> Uninstall -> Re-install with mask/code -> Start).
     */
    static bool setHardwareFilter(bool enableHardwareFilter, uint32_t filterAcceptanceCode, uint32_t filterAcceptanceMask, bool isExtendedFrame);

    /**
     * @brief Returns true when the TWAI UDS Hardware Filter (0x7C0-0x7FF) is active.
     */
    static bool isHardwareFilterEnabled();

    /**
     * @brief Polls TWAI status registers and initiates automatic Bus-Off recovery.
     */
    static void checkAlertsAndRecover();

    /**
     * @brief Controls the TJA1043 Normal vs Standby operating modes (STB_N/EN pins,
     *        configurable via settings). No-op when settings.can_chip_type is CAN_CHIP_PLAIN,
     *        since a plain transceiver has no standby control lines.
     *        Normal  : STB_N = HIGH, EN = HIGH
     *        Standby : STB_N = LOW,  EN = LOW
     */
    static void setTransceiverSleep(bool enterStandbySleep);

    /**
     * @brief Pulses the CAN Activity LED (settings.tja_led_pin) and, when the active chip
     *        is the TJA1043, manages its inactivity standby sleep countdown.
     */
    static void serviceTransceiverPowerAndLed();
};
