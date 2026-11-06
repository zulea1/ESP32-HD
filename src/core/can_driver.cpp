/**
 * ============================================================================
 * File      : src/core/can_driver.cpp
 * Project   : ESP32 HD - UDS Diagnostic Service Tester (ISO 14229 / ISO 15765-2)
 * Copyright : (c) 2026 Henzulea Daniel email: <zulea1@gmail.com>
 * License   : GNU General Public License v3.0 or later (GPL-3.0-or-later) - see LICENSE
 * Version   : see FIRMWARE_VERSION_STR / FIRMWARE_BUILD_NUM_STR in src/config.h
 * ============================================================================
 * HARDWARE ARCHITECTURE & DRIVER IMPLEMENTATION NOTES:
 * 1. TJA1043 High-Speed CAN Transceiver State Machine:
 *    - Normal Mode  : STB_N = HIGH (GPIO 19), EN = HIGH (GPIO 18)
 *    - Standby Mode : STB_N = LOW  (GPIO 19), EN = LOW  (GPIO 18)
 *    - Fault Output : ERR_N (GPIO 33, Active LOW with internal pull-up)
 *
 * 2. Cross-Core Thread-Safe Transmission (`CanDriver::transmit`):
 *    - Protected by `canTxMutex` so Core 0 (ISO-TP Flow Control 0x30 and
 *      background TesterPresent 0x3E) and Core 1 (WebServer UDS 0x22 / 0x19 /
 *      0x14 requests) never collide on the SJA1000 TX buffer.
 *    - Automatically checks TWAI state before each transmit attempt and
 *      re-starts or recovers the controller if a bus error occurred.
 *
 * 3. Non-Blocking Hardware Burst Extraction (`CanDriver::receiveBurst`):
 *    - Configured with a 128-slot hardware RX queue (`twaiGeneralConfig.rx_queue_len = 128`).
 *    - `receiveBurst()` loops up to 32 times calling `twai_receive(&receivedFrameBuffer[receivedFrameCount], 0)`
 *      with a 0-tick timeout, extracting all queued CAN frames in microseconds.
 *    - TWAI HW Filter defaults to ON after reboot, covering UDS IDs 0x7C0 to 0x7FF -
 *      this is the real matched range (see HD_TWAI_FILTER_LO/HI below), not just
 *      0x7D0 upward; it includes SCM's 0x7D0/0x7D8 as well as every 0x7Ex/0x7Fx module.
 * ============================================================================
 */
#include "can_driver.h"
#include "config.h"

// Default SJA1000 single-filter range: every HD diagnostic request/response ID lives in
// this 64-ID block, SCM's 0x7D0 (request) / 0x7D8 (response) included. Keep LO at 0x7C0: it's the
// power-of-two-aligned base
// the SJA1000 single filter needs, not an arbitrary cutoff above SCM's address.
#define HD_TWAI_FILTER_LO 0x7C0UL
#define HD_TWAI_FILTER_HI 0x7FFUL

static uint32_t currentBusBitrate          = DEFAULT_CAN_BITRATE;
static uint32_t activityLedOffDeadlineMillis = 0;
static bool     hardwareFilterEnabled      = true; // Default ON after reboot (UDS IDs 0x7C0..0x7FF - see HD_TWAI_FILTER_LO/HI)
static SemaphoreHandle_t canTxMutex        = NULL;

/**
 * @brief Returns the ESP-IDF TWAI timing configuration structure for the
 *        requested bus baudrate (125 kbps, 250 kbps, 500 kbps, or 1 Mbps).
 */
static twai_timing_config_t getTimingConfig(uint32_t requestedBusBitrate) {
    switch (requestedBusBitrate) {
        case 125000:  return TWAI_TIMING_CONFIG_125KBITS();
        case 250000:  return TWAI_TIMING_CONFIG_250KBITS();
        case 500000:  return TWAI_TIMING_CONFIG_500KBITS();
        case 1000000: return TWAI_TIMING_CONFIG_1MBITS();
        default:      return TWAI_TIMING_CONFIG_500KBITS();
    }
}

void CanDriver::setTransceiverSleep(bool enterStandbySleep) {
    // Standby/sleep control only exists on the TJA1043's STB_N/EN pins. A plain RX/TX
    // transceiver has no such control line, so there is nothing to drive - just reflect
    // the requested state in metrics and return (serviceTransceiverPowerAndLed() also
    // never schedules a sleep timeout in plain mode, so enterStandbySleep==true should
    // not normally occur here in that mode, but this guard keeps the function safe anyway).
    if (settings.can_chip_type != CAN_CHIP_TJA1043) {
        metrics.tja_sleeping = false;
        return;
    }

    if (enterStandbySleep) {
        digitalWrite(settings.tja_stb_pin, LOW);
        digitalWrite(settings.tja_en_pin, LOW);
        metrics.tja_sleeping = true;
        Serial.println("[TJA1043] Transceiver switched to STANDBY mode (STB_N=LOW, EN=LOW).");
    } else {
        digitalWrite(settings.tja_stb_pin, HIGH);
        digitalWrite(settings.tja_en_pin, HIGH);
        delayMicroseconds(150); // Allow TJA1043 internal charge pump to stabilize
        metrics.tja_sleeping = false;
        Serial.println("[TJA1043] Transceiver woke to NORMAL ACTIVE mode (STB_N=HIGH, EN=HIGH).");
    }
}

bool CanDriver::init(uint32_t canBusBitrate) {
    currentBusBitrate     = canBusBitrate;
    hardwareFilterEnabled = true; // Default ON after reboot (0x7C0..0x7FF)

    if (canTxMutex == NULL) {
        canTxMutex = xSemaphoreCreateMutex();
    }

    metrics.tja_sleeping = false;
    metrics.tja_fault    = false;

    if (settings.can_chip_type == CAN_CHIP_TJA1043) {
        Serial.printf("[CAN] Configuring TJA1043 pins (STB=GPIO%d, EN=GPIO%d, ERR=GPIO%d, LED=GPIO%d)...\n",
                      settings.tja_stb_pin, settings.tja_en_pin, settings.tja_err_pin, CAN_ACTIVITY_LED_PIN);

        // Configure TJA1043 mode control & diagnostic pins
        pinMode(settings.tja_stb_pin, OUTPUT);
        pinMode(settings.tja_en_pin, OUTPUT);
        pinMode(settings.tja_err_pin, INPUT_PULLUP);
        pinMode(CAN_ACTIVITY_LED_PIN, OUTPUT);
        digitalWrite(CAN_ACTIVITY_LED_PIN, LOW);

        // Wake TJA1043 into Normal Active Mode
        setTransceiverSleep(false);
    } else {
        Serial.printf("[CAN] Configuring plain CAN transceiver (TX=GPIO%d, RX=GPIO%d, LED=GPIO%d) - no STB/EN/ERR control lines.\n",
                      settings.can_tx_pin, settings.can_rx_pin, CAN_ACTIVITY_LED_PIN);

        // Plain transceivers have no standby/enable/fault lines - only the activity LED applies.
        pinMode(CAN_ACTIVITY_LED_PIN, OUTPUT);
        digitalWrite(CAN_ACTIVITY_LED_PIN, LOW);
    }
    metrics.last_can_activity_ms = millis();

    // Configure ESP32 TWAI General Settings (128 RX slots, 64 TX slots)
    twai_general_config_t twaiGeneralConfig = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)settings.can_tx_pin,
        (gpio_num_t)settings.can_rx_pin,
        TWAI_MODE_NORMAL
    );
    twaiGeneralConfig.rx_queue_len = 128;
    twaiGeneralConfig.tx_queue_len = 64;

    twai_timing_config_t twaiTimingConfig = getTimingConfig(canBusBitrate);
    twai_filter_config_t twaiFilterConfig = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    // Apply default ON SJA1000 silicon filter covering the full HD diagnostic ID block
    // 0x7C0..0x7FF (64 consecutive standard IDs). This is the actual matched range, not just
    // the top end of it - it includes every module on this bus, SCM's 0x7D0 request / 0x7D8
    // response included, even though SCM sits lower than the 0x7Ex/0x7Fx modules.
    // A single SJA1000 filter can only match a power-of-two-aligned block via a prefix/mask
    // pair, so HD_TWAI_FILTER_LO below (0x7C0) is that block's base, not an arbitrary cutoff.
    twaiFilterConfig.single_filter   = true;
    twaiFilterConfig.acceptance_code = (HD_TWAI_FILTER_LO & 0x7FFUL) << 21;
    twaiFilterConfig.acceptance_mask = ~((HD_TWAI_FILTER_LO & 0x7FFUL) << 21);
    Serial.printf("[CAN] HW acceptance filter active: 0x%03lX-0x%03lX (covers SCM 0x7D0/0x7D8 and every ECM/ABS/BCM/LHC/RHC/EHCU/RAD/SPDO ID)\n",
                  (unsigned long)HD_TWAI_FILTER_LO, (unsigned long)HD_TWAI_FILTER_HI);

    if (twai_driver_install(&twaiGeneralConfig, &twaiTimingConfig, &twaiFilterConfig) != ESP_OK) {
        Serial.println("[CAN] CRITICAL ERROR: Failed to install ESP32 TWAI driver!");
        return false;
    }
    if (twai_start() != ESP_OK) {
        Serial.println("[CAN] CRITICAL ERROR: Failed to start ESP32 TWAI controller!");
        return false;
    }

    metrics.twai_state = 1; // RUNNING
    Serial.printf("[CAN] TWAI Controller Started @ %u bps | TX=GPIO%d, RX=GPIO%d | RX_Queue=128, TX_Queue=64 | HW Filter: ON (0x7C0-0x7FF)\n",
                  canBusBitrate, settings.can_tx_pin, settings.can_rx_pin);
    return true;
}

bool CanDriver::transmit(uint32_t canIdentifier, bool isExtendedFrame, bool isRemoteFrame, uint8_t dataLengthCode, const uint8_t *frameDataBytes) {
    bool transmitMutexLockAcquired = false;
    if (canTxMutex != NULL) {
        transmitMutexLockAcquired = (xSemaphoreTake(canTxMutex, pdMS_TO_TICKS(100)) == pdTRUE);
    }

    // Wake TJA1043 immediately if currently in Standby Sleep Mode
    if (metrics.tja_sleeping) {
        setTransceiverSleep(false);
    }

    twai_message_t outgoingCanMessage = {};
    outgoingCanMessage.identifier       = canIdentifier;
    outgoingCanMessage.extd             = isExtendedFrame ? 1 : 0;
    outgoingCanMessage.rtr              = isRemoteFrame ? 1 : 0;
    outgoingCanMessage.data_length_code = (dataLengthCode > 8) ? 8 : dataLengthCode;

    if (!isRemoteFrame && frameDataBytes != nullptr && outgoingCanMessage.data_length_code > 0) {
        memcpy(outgoingCanMessage.data, frameDataBytes, outgoingCanMessage.data_length_code);
    }

    // Ensure adequate queue wait time (50ms / at least 2 FreeRTOS ticks) so frame is not dropped during arbitration
    TickType_t transmitWaitTicks = pdMS_TO_TICKS(50);
    if (transmitWaitTicks < 2) transmitWaitTicks = 2;

    esp_err_t transmitResult = ESP_FAIL;
    for (int attemptIndex = 0; attemptIndex < 4 && transmitResult != ESP_OK; attemptIndex++) {
        twai_status_info_t twaiStatusInfo;
        if (twai_get_status_info(&twaiStatusInfo) == ESP_OK) {
            if (twaiStatusInfo.state == TWAI_STATE_STOPPED) {
                Serial.println("[CAN] Notice: TWAI was STOPPED prior to TX -> Restarting TWAI controller...");
                twai_start();
            } else if (twaiStatusInfo.state == TWAI_STATE_BUS_OFF) {
                Serial.println("[CAN] WARNING: TWAI in BUS-OFF prior to TX -> Initiating recovery...");
                twai_initiate_recovery();
                vTaskDelay(pdMS_TO_TICKS(15) > 0 ? pdMS_TO_TICKS(15) : 2);
                if (twai_get_status_info(&twaiStatusInfo) == ESP_OK && twaiStatusInfo.state == TWAI_STATE_STOPPED) {
                    twai_start();
                }
            } else if (twaiStatusInfo.state == TWAI_STATE_RECOVERING) {
                vTaskDelay(pdMS_TO_TICKS(10) > 0 ? pdMS_TO_TICKS(10) : 1);
                if (twai_get_status_info(&twaiStatusInfo) == ESP_OK && twaiStatusInfo.state == TWAI_STATE_STOPPED) {
                    twai_start();
                }
            }
        }

        transmitResult = twai_transmit(&outgoingCanMessage, transmitWaitTicks);
        if (transmitResult != ESP_OK && attemptIndex < 3) {
            Serial.printf("[CAN] TX retry #%d for CAN ID 0x%03X (err=0x%X)...\n", attemptIndex + 1, canIdentifier, (unsigned int)transmitResult);
            vTaskDelay(pdMS_TO_TICKS(10) > 0 ? pdMS_TO_TICKS(10) : 1);
        }
    }

    if (transmitMutexLockAcquired) {
        xSemaphoreGive(canTxMutex);
    }

    if (transmitResult == ESP_OK) {
        uint32_t currentMillis = millis();
        metrics.last_can_activity_ms = currentMillis;
        digitalWrite(CAN_ACTIVITY_LED_PIN, HIGH);
        activityLedOffDeadlineMillis = currentMillis + 15;

        if (xSemaphoreTake(metricsMutex, 0) == pdTRUE) {
            metrics.can_tx_count++;
            xSemaphoreGive(metricsMutex);
        }
        return true;
    }

    Serial.printf("[CAN] ERROR: TX failed for CAN ID 0x%03X after 4 attempts (err=0x%X).\n", canIdentifier, (unsigned int)transmitResult);
    return false;
}

size_t CanDriver::receiveBurst(twai_message_t *receivedFrameBuffer, size_t maximumFrameCount, uint32_t timeoutMilliseconds) {
    size_t receivedFrameCount = 0;
    TickType_t initialWaitTicks = (timeoutMilliseconds == 0) ? 0 : pdMS_TO_TICKS(timeoutMilliseconds);
    bool isFirstReceiveAttempt = true;

    while (receivedFrameCount < maximumFrameCount) {
        TickType_t receiveWaitTicks = isFirstReceiveAttempt ? initialWaitTicks : 0;
        isFirstReceiveAttempt = false;
        twai_message_t receivedCanMessage;
        if (twai_receive(&receivedCanMessage, receiveWaitTicks) == ESP_OK) {
            // Enforce exact HD_TWAI_FILTER_LO..HD_TWAI_FILTER_HI (0x7C0..0x7FF) UDS range when
            // TWAI HW Filter is ON - must match the actual silicon filter configured in init()/
            // setHardwareFilter(), not a stale narrower bound (previously 0x7D0, which incorrectly
            // dropped valid 0x7C0-0x7CF frames the hardware filter already lets through).
            if (hardwareFilterEnabled && (receivedCanMessage.extd || receivedCanMessage.identifier < HD_TWAI_FILTER_LO || receivedCanMessage.identifier > HD_TWAI_FILTER_HI)) {
                continue;
            }
            receivedFrameBuffer[receivedFrameCount++] = receivedCanMessage;
        } else {
            break; // Hardware FIFO is now completely empty
        }
    }

    if (receivedFrameCount > 0) {
        uint32_t currentMillis = millis();
        metrics.last_can_activity_ms = currentMillis;
        if (metrics.tja_sleeping) {
            setTransceiverSleep(false);
        }
        digitalWrite(CAN_ACTIVITY_LED_PIN, HIGH);
        activityLedOffDeadlineMillis = currentMillis + 15;
    }

    return receivedFrameCount;
}

bool CanDriver::setHardwareFilter(bool enableHardwareFilter, uint32_t filterAcceptanceCode, uint32_t filterAcceptanceMask, bool isExtendedFrame) {
    hardwareFilterEnabled = enableHardwareFilter;
    Serial.printf("[CAN] Reconfiguring SJA1000 HW Acceptance Filter: Enable=%s, Code=0x%X, Mask=0x%X\n",
                  enableHardwareFilter ? "ON" : "OFF", filterAcceptanceCode, filterAcceptanceMask);
    twai_stop();
    twai_driver_uninstall();

    twai_general_config_t twaiGeneralConfig = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)settings.can_tx_pin,
        (gpio_num_t)settings.can_rx_pin,
        TWAI_MODE_NORMAL
    );
    twaiGeneralConfig.rx_queue_len = 128;
    twaiGeneralConfig.tx_queue_len = 64;

    twai_timing_config_t twaiTimingConfig = getTimingConfig(currentBusBitrate);
    twai_filter_config_t twaiFilterConfig = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (enableHardwareFilter) {
        twaiFilterConfig.single_filter = true;
        if (isExtendedFrame) {
            twaiFilterConfig.acceptance_code = (filterAcceptanceCode & 0x1FFFFFFF) << 3;
            twaiFilterConfig.acceptance_mask = filterAcceptanceMask;
        } else {
            twaiFilterConfig.acceptance_code = (filterAcceptanceCode & 0x7FF) << 21;
            twaiFilterConfig.acceptance_mask = (filterAcceptanceMask != 0) ? (~((filterAcceptanceMask & 0x7FFUL) << 21)) : (~(HD_TWAI_FILTER_LO << 21));
        }
    }

    if (twai_driver_install(&twaiGeneralConfig, &twaiTimingConfig, &twaiFilterConfig) != ESP_OK) return false;
    if (twai_start() != ESP_OK) return false;
    return true;
}

bool CanDriver::isHardwareFilterEnabled() {
    return hardwareFilterEnabled;
}

void CanDriver::checkAlertsAndRecover() {
    twai_status_info_t twaiStatusInfo;
    if (twai_get_status_info(&twaiStatusInfo) != ESP_OK) return;

    // ERR_N fault sensing only exists on the TJA1043 - a plain transceiver has no fault pin.
    bool transceiverFaultPinLow = (settings.can_chip_type == CAN_CHIP_TJA1043) &&
                                   (digitalRead(settings.tja_err_pin) == LOW);

    if (xSemaphoreTake(metricsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        metrics.twai_state    = (uint8_t)twaiStatusInfo.state;
        metrics.can_err_count = twaiStatusInfo.bus_error_count;
        metrics.tja_fault     = transceiverFaultPinLow;
        xSemaphoreGive(metricsMutex);
    }

    if (twaiStatusInfo.state == TWAI_STATE_BUS_OFF) {
        Serial.printf("[CAN] ALERT: TWAI Controller entered BUS-OFF (TX_Err=%u, RX_Err=%u)! Initiating recovery...\n",
                      twaiStatusInfo.tx_error_counter, twaiStatusInfo.rx_error_counter);
        twai_initiate_recovery();
    } else if (twaiStatusInfo.state == TWAI_STATE_STOPPED) {
        Serial.println("[CAN] TWAI recovery complete -> Restarting TWAI controller into RUNNING state.");
        twai_start();
    }
}

void CanDriver::serviceTransceiverPowerAndLed() {
    uint32_t currentMillis = millis();

    // Extinguish activity LED pulse after 15ms (LED is a visual indicator only, independent
    // of transceiver chip type, so this stays active for both TJA1043 and plain mode)
    if (activityLedOffDeadlineMillis != 0 && (int32_t)(currentMillis - activityLedOffDeadlineMillis) >= 0) {
        digitalWrite(CAN_ACTIVITY_LED_PIN, LOW);
        activityLedOffDeadlineMillis = 0;
    }

    // Check TJA1043 Inactivity Sleep Timeout (only if timeout > 0s). Standby sleep only
    // applies when a TJA1043 is actually in use - a plain transceiver has no STB/EN pins to sleep.
    if (settings.can_chip_type == CAN_CHIP_TJA1043 &&
        settings.tja_sleep_enable && settings.tja_sleep_timeout_sec > 0 && !metrics.tja_sleeping) {
        uint32_t secondsSinceLastActivity = (currentMillis - metrics.last_can_activity_ms) / 1000;
        if (secondsSinceLastActivity >= settings.tja_sleep_timeout_sec) {
            Serial.printf("[TJA1043] No CAN activity for %u s. Entering Standby Sleep Mode.\n",
                          secondsSinceLastActivity);
            setTransceiverSleep(true);
        }
    }
}
