/**
 * ============================================================================
 * File     : src/core/web_server.cpp
 * Project  : ESP32 HD - UDS Diagnostic Service Tester (ISO 14229 / ISO 15765-2)
 * Copyright: (c) 2026 Henzulea Daniel email: <zulea1@gmail.com>
 * License  : GNU General Public License v3.0 or later (GPL-3.0-or-later) - see LICENSE
 * Version   : see FIRMWARE_VERSION_STR / FIRMWARE_BUILD_NUM_STR in src/config.h
 * ============================================================================
 * ARCHITECTURAL OVERVIEW:
 *   Implements the HTTP REST API, WebSocket (`/ws`) telemetry broadcaster,
 *   and embedded Web UI server on Core 1 (Port 80).
 *
 *   1. ZERO-HEAP PROGMEM STREAMING:
 *      - All HTML, CSS, and JavaScript assets are compiled into flash memory
 *        (`PROGMEM`) by `build_web_ui.py` prior to compilation.
 *      - `sendProgmemAsset()` streams assets directly from flash in 4096-byte
 *        chunks, preventing heap fragmentation during concurrent UDS queries.
 *
 *   2. ECM, ABS & BCM DID 0x0200 LIVE TELEMETRY & WEBSOCKET STREAMING (`/ws`):
 *      - `serializeEcmInputs0200Json()`, `serializeAbsInputs0200Json()`, and
 *        `serializeBcmInputs0200Json()` serialize live parameters from DID 0x0200
 *        into static Core 1 JSON buffers (eliminating stack overflow panics).
 *      - Pushes real-time updates over `/ws` whenever a live stream update occurs.
 *
 *   3. ECM (0x7E0) & ABS (0x7E1) UDS 0x2F ACTUATOR CONTROL & FAILSAFE CLEANUP:
 *      - Processes WebSocket JSON commands:
 *        * `{"action":"control_abs_output","did":"0540","state":1}` (ABS 0x7E1)
 *        * `{"action":"enter_abs_outputs"}` / `{"action":"cleanup_abs_outputs"}`
 *        * `{"action":"control_output","did":"0502","state":1}` (ECM 0x7E0)
 *        * `{"action":"enter_outputs"}` / `{"action":"cleanup_outputs"}`
 *      - Services non-blocking `millis()` auto-reset timers for Pulse/Test Output DIDs
 *        (including 1000ms auto-reset `04 2F 05 40 00 00 00 00` for ABS Pump Motor).
 *      - Invokes `UdsHandler::cleanupAbsActuatorOutputs()` or `cleanupAllActuatorOutputs()`
 *        on page exit or WebSocket disconnect.
 * ============================================================================
 */

#include "web_server.h"
#include "config.h"
#include "can_driver.h"
#include "uds_handler.h"
#include "ecm_outputs.h"
#include "bcm_outputs.h"
#include "abs_outputs.h"
#include "spdo_outputs.h"
#include "abs_special.h"
#include "bcm_special.h"
#include "spdo_special.h"
#include "rad_special.h"
#include "ecm_special.h"
#include "lhc_special.h"
#include "ecu_info.h"
#include "safe_boot.h"
#include "hd_dtc_db.h"      // lookupHDDtcDescription() for the /api/dtc/lookup reference panel
#include "proc_runner.h"    // generic guided-procedure engine (/api/proc/*)
#include "ecm_algo.h"       // ecmComputeSecurityKey() - reused for the LHC/RHC hand-control unlock (mode 0x0B)
#include "bcm_algo.h"       // bcmComputeSecurityKey() for the PIN read endpoint

// Auto-generated PROGMEM headers produced by build_web_ui.py
#include "web_ui/index_html.h"
#include "web_ui/style_css.h"
#include "web_ui/main_js.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <Update.h>
#include "ota_helper.h"

static WebServer server(80);

// Escapes a C string for safe embedding in a JSON string literal.
static void jsonEscapeInto(char *destinationBuffer, size_t destinationCapacity, const char *sourceText) {
    size_t writtenOffset = 0;
    if (destinationCapacity == 0) return;
    for (; sourceText && *sourceText && writtenOffset + 7 < destinationCapacity; ++sourceText) {
        unsigned char currentChar = (unsigned char)*sourceText;
        if (currentChar == '"' || currentChar == '\\') { destinationBuffer[writtenOffset++] = '\\'; destinationBuffer[writtenOffset++] = (char)currentChar; }
        else if (currentChar < 0x20)        { writtenOffset += snprintf(destinationBuffer + writtenOffset, destinationCapacity - writtenOffset, "\\u%04x", currentChar); }
        else                      { destinationBuffer[writtenOffset++] = (char)currentChar; }
    }
    destinationBuffer[writtenOffset] = '\0';
}

// Connected RFC 6455 WebSocket clients upgraded on "/ws"
static const size_t MAX_WS_CLIENTS = 4;
static WiFiClient   wsClients[MAX_WS_CLIENTS];
static bool         wsClientWasConnected[MAX_WS_CLIENTS] = {false, false, false, false};
static volatile bool     live_stream_0200_enabled = false;
static volatile uint32_t live_stream_0200_module  = HD_ECU_ECM;
static volatile bool     s_any_ws_client          = false;   // refreshed by the web loop, read by the poller task
static bool         outputs_view_active      = false;
static bool         abs_outputs_view_active  = false;
static volatile uint32_t last_live_stream_0200_ms = 0;
static uint32_t     last_voltage_check_ms    = 0;

// Static Core 1 work structs & JSON buffers (prevents FreeRTOS loopTask stack overflow)
static const size_t ECM_0200_JSON_BUFFER_SIZE = 2560;
static const size_t ABS_0200_JSON_BUFFER_SIZE = 1536;
static const size_t BCM_0200_JSON_BUFFER_SIZE = 1536;

static EcmInputs0200   s_web_ecm_inputs;
static AbsInputs0200   s_web_abs_inputs;
static BcmInputs0200   s_web_bcm_inputs;
static HDDtcRecord s_web_dtcs[16];

// ---- Live DID 0x0200 poller task (own buffers; the web loop only broadcasts finished frames) ----
static EcmInputs0200     s_poll_ecm;
static AbsInputs0200     s_poll_abs;
static BcmInputs0200     s_poll_bcm;
static char              s_stream_json[ECM_0200_JSON_BUFFER_SIZE];
static GenInputs0200     s_poll_gen;
static GenInputs0200     s_web_gen_inputs;
static char              s_http_gen_json[2048];
static size_t            s_stream_len        = 0;
static volatile uint32_t s_stream_seq        = 0;   // incremented by the poller per published frame
static uint32_t          s_stream_sent_seq   = 0;   // owned by the web loop
static SemaphoreHandle_t s_stream_mutex      = nullptr;
static TaskHandle_t      s_poll_task_handle  = nullptr;
static char            s_ws_ecm_json[ECM_0200_JSON_BUFFER_SIZE];
static char            s_http_ecm_json[ECM_0200_JSON_BUFFER_SIZE];
static char            s_ws_abs_json[ABS_0200_JSON_BUFFER_SIZE];
static char            s_http_abs_json[ABS_0200_JSON_BUFFER_SIZE];
static char            s_ws_bcm_json[BCM_0200_JSON_BUFFER_SIZE];
static char            s_http_bcm_json[BCM_0200_JSON_BUFFER_SIZE];

// ============================================================================
// Self-Contained RFC 3174 SHA-1 & RFC 4648 Base64 for WebSocket Handshake
// (Eliminates external mbedtls_sha1 linker symbol differences across ESP-IDF 4/5)
// ============================================================================
static inline uint32_t sha1Rol32(uint32_t value, unsigned int bits) {
    return (value << bits) | (value >> (32U - bits));
}

static void computeRfc6455Sha1(const uint8_t *inputData, size_t inputLength, uint8_t outputHashBytes[20]) {
    uint32_t hashWord0 = 0x67452301UL;
    uint32_t hashWord1 = 0xEFCDAB89UL;
    uint32_t hashWord2 = 0x98BADCFEUL;
    uint32_t hashWord3 = 0x10325476UL;
    uint32_t hashWord4 = 0xC3D2E1F0UL;

    // WebSocket handshake input is ~60 bytes; 128-byte buffer handles up to 119 bytes (2 blocks)
    uint8_t paddedMessage[128];
    memset(paddedMessage, 0, sizeof(paddedMessage));
    if (inputLength > 119) inputLength = 119;
    memcpy(paddedMessage, inputData, inputLength);
    paddedMessage[inputLength] = 0x80;

    size_t totalBlockCount = ((inputLength + 8) >= 64) ? 2 : 1;
    size_t bitLengthOffset = (totalBlockCount * 64) - 8;
    uint64_t messageBitLength = (uint64_t)inputLength * 8ULL;
    for (int byteIndex = 0; byteIndex < 8; byteIndex++) {
        paddedMessage[bitLengthOffset + byteIndex] = (uint8_t)((messageBitLength >> (56 - (byteIndex * 8))) & 0xFFU);
    }

    for (size_t blockIndex = 0; blockIndex < totalBlockCount; blockIndex++) {
        uint32_t messageSchedule[80];
        const uint8_t *blockBytes = paddedMessage + (blockIndex * 64);
        for (int wordIndex = 0; wordIndex < 16; wordIndex++) {
            messageSchedule[wordIndex] = ((uint32_t)blockBytes[wordIndex * 4 + 0] << 24) |
                   ((uint32_t)blockBytes[wordIndex * 4 + 1] << 16) |
                   ((uint32_t)blockBytes[wordIndex * 4 + 2] << 8)  |
                   ((uint32_t)blockBytes[wordIndex * 4 + 3]);
        }
        for (int wordIndex = 16; wordIndex < 80; wordIndex++) {
            messageSchedule[wordIndex] = sha1Rol32(messageSchedule[wordIndex - 3] ^ messageSchedule[wordIndex - 8] ^ messageSchedule[wordIndex - 14] ^ messageSchedule[wordIndex - 16], 1);
        }

        uint32_t roundVarA = hashWord0;
        uint32_t roundVarB = hashWord1;
        uint32_t roundVarC = hashWord2;
        uint32_t roundVarD = hashWord3;
        uint32_t roundVarE = hashWord4;

        for (int roundIndex = 0; roundIndex < 80; roundIndex++) {
            uint32_t mixFunctionResult = 0;
            uint32_t roundConstant = 0;
            if (roundIndex < 20) {
                mixFunctionResult = (roundVarB & roundVarC) | ((~roundVarB) & roundVarD);
                roundConstant = 0x5A827999UL;
            } else if (roundIndex < 40) {
                mixFunctionResult = roundVarB ^ roundVarC ^ roundVarD;
                roundConstant = 0x6ED9EBA1UL;
            } else if (roundIndex < 60) {
                mixFunctionResult = (roundVarB & roundVarC) | (roundVarB & roundVarD) | (roundVarC & roundVarD);
                roundConstant = 0x8F1BBCDCUL;
            } else {
                mixFunctionResult = roundVarB ^ roundVarC ^ roundVarD;
                roundConstant = 0xCA62C1D6UL;
            }
            uint32_t rotatedSum = sha1Rol32(roundVarA, 5) + mixFunctionResult + roundVarE + roundConstant + messageSchedule[roundIndex];
            roundVarE = roundVarD;
            roundVarD = roundVarC;
            roundVarC = sha1Rol32(roundVarB, 30);
            roundVarB = roundVarA;
            roundVarA = rotatedSum;
        }

        hashWord0 += roundVarA;
        hashWord1 += roundVarB;
        hashWord2 += roundVarC;
        hashWord3 += roundVarD;
        hashWord4 += roundVarE;
    }

    uint32_t finalHashWords[5] = {hashWord0, hashWord1, hashWord2, hashWord3, hashWord4};
    for (int wordIndex = 0; wordIndex < 5; wordIndex++) {
        outputHashBytes[wordIndex * 4 + 0] = (uint8_t)((finalHashWords[wordIndex] >> 24) & 0xFFU);
        outputHashBytes[wordIndex * 4 + 1] = (uint8_t)((finalHashWords[wordIndex] >> 16) & 0xFFU);
        outputHashBytes[wordIndex * 4 + 2] = (uint8_t)((finalHashWords[wordIndex] >> 8)  & 0xFFU);
        outputHashBytes[wordIndex * 4 + 3] = (uint8_t)(finalHashWords[wordIndex] & 0xFFU);
    }
}

static void encodeRfc6455Base64(const uint8_t *sourceBytes, size_t sourceLength, char *outputString, size_t maxOutputLength) {
    static const char base64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t outputIndex = 0;
    for (size_t sourceIndex = 0; sourceIndex < sourceLength && (outputIndex + 4) < maxOutputLength; sourceIndex += 3) {
        uint32_t tripletValue = ((uint32_t)sourceBytes[sourceIndex]) << 16;
        if (sourceIndex + 1 < sourceLength) tripletValue |= ((uint32_t)sourceBytes[sourceIndex + 1]) << 8;
        if (sourceIndex + 2 < sourceLength) tripletValue |= ((uint32_t)sourceBytes[sourceIndex + 2]);

        outputString[outputIndex++] = base64Alphabet[(tripletValue >> 18) & 0x3FU];
        outputString[outputIndex++] = base64Alphabet[(tripletValue >> 12) & 0x3FU];
        outputString[outputIndex++] = (sourceIndex + 1 < sourceLength) ? base64Alphabet[(tripletValue >> 6) & 0x3FU] : '=';
        outputString[outputIndex++] = (sourceIndex + 2 < sourceLength) ? base64Alphabet[tripletValue & 0x3FU] : '=';
    }
    if (outputIndex < maxOutputLength) {
        outputString[outputIndex] = '\0';
    }
}

// ============================================================================
// Helper: Map HD CAN Request ID to Module Acronym
// ============================================================================
static const char* lookupModuleName(uint32_t canId) {
    switch (canId) {
        case HD_ECU_SCM:    return "SCM";
        case HD_ECU_EHCU:   return "EHCU";
        case HD_ECU_ECM:    return "ECM";
        case HD_ECU_ABS:    return "ABS";
        case HD_ECU_BCM:    return "BCM";
        case HD_ECU_LHC:    return "LHC";
        case HD_ECU_RHC:    return "RHC";
        case HD_ECU_RADIO:  return "RAD";
        case HD_ECU_SPEEDO: return "SPDO";
        default:                return "";
    }
}

// ============================================================================
// Helper: Stream Flash-Resident PROGMEM Assets in 4 KB Chunks
// ============================================================================
static void sendProgmemAsset(const char *mimeType, const uint8_t *gzData, size_t totalLen) {
    // ETag = firmware version + build + flash stamp + asset length -> browser revalidates (304) instead of re-downloading.
    char etag[96];
    snprintf(etag, sizeof(etag), "\"%s-%s-%s-%u\"", FIRMWARE_VERSION_STR, FIRMWARE_BUILD_NUM_STR, SERIAL_FLASH_STAMP, (unsigned)totalLen);
    server.sendHeader("ETag", etag);
    server.sendHeader("Cache-Control", "no-cache, max-age=0, must-revalidate");
    if (server.hasHeader("If-None-Match") && server.header("If-None-Match") == etag) {
        server.send(304, mimeType, "");
        return;
    }
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Vary", "Accept-Encoding");
    server.setContentLength(totalLen);
    server.send(200, mimeType, "");

    const size_t CHUNK_SIZE = 4096;
    size_t offset = 0;
    while (offset < totalLen) {
        size_t chunk = totalLen - offset;
        if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;
        server.sendContent((const char *)(gzData + offset), chunk);
        offset += chunk;
    }
}

// ============================================================================
// RFC 6455 WebSocket Frame Transmitter & JSON Action Dispatcher
// ============================================================================
static void sendWsTextFrame(WiFiClient &client, const char *payload, size_t len) {
    if (!client.connected() || payload == nullptr || len == 0) return;

    uint8_t header[10];
    size_t headerLen = 0;
    header[0] = 0x81; // FIN=1, Opcode=0x1 (Text Frame)

    if (len <= 125) {
        header[1] = (uint8_t)len;
        headerLen = 2;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (uint8_t)((len >> 8) & 0xFF);
        header[3] = (uint8_t)(len & 0xFF);
        headerLen = 4;
    } else {
        return;
    }

    client.write(header, headerLen);
    client.write((const uint8_t *)payload, len);
}

void MainWebServer::broadcastWsJson(const char *jsonPayload, size_t payloadLength) {
    if (jsonPayload == nullptr) return;
    if (payloadLength == 0) payloadLength = strlen(jsonPayload);
    if (payloadLength == 0) return;

    for (size_t clientIndex = 0; clientIndex < MAX_WS_CLIENTS; clientIndex++) {
        if (wsClients[clientIndex] && wsClients[clientIndex].connected()) {
            sendWsTextFrame(wsClients[clientIndex], jsonPayload, payloadLength);
        }
    }
}

// Returns pointer to the value following  "key" <ws> : <ws> ["]  or nullptr if absent/malformed.
static const char *jsonValueAfterKey(const char *jsonText, const char *quotedKey) {
    const char *keyPosition = strstr(jsonText, quotedKey);
    if (keyPosition == nullptr) return nullptr;
    const char *valuePosition = keyPosition + strlen(quotedKey);
    while (*valuePosition == ' ' || *valuePosition == '\t' || *valuePosition == '\r' || *valuePosition == '\n') valuePosition++;
    if (*valuePosition != ':') return nullptr;
    valuePosition++;
    while (*valuePosition == ' ' || *valuePosition == '\t' || *valuePosition == '\r' || *valuePosition == '\n' || *valuePosition == '"') valuePosition++;
    return valuePosition;
}

static bool extractJsonDidAndState(const char *jsonText, uint16_t &outDid, uint8_t &outState) {
    outDid   = HD_DID_ACT_FUEL_PUMP;
    outState = 1;
    if (jsonText == nullptr) return false;

    const char *valueText = jsonValueAfterKey(jsonText, "\"did\"");
    if (valueText != nullptr) {
        if (valueText[0] == '0' && (valueText[1] == 'x' || valueText[1] == 'X')) valueText += 2;
        char *parseEndPointer = nullptr;
        unsigned long parsedDid = strtoul(valueText, &parseEndPointer, 16);
        if (parseEndPointer != valueText && parsedDid <= 0xFFFFUL) outDid = (uint16_t)parsedDid;
    }

    valueText = jsonValueAfterKey(jsonText, "\"state\"");
    if (valueText != nullptr) {
        char *parseEndPointer = nullptr;
        unsigned long parsedState = strtoul(valueText, &parseEndPointer, 10);
        if (parseEndPointer != valueText && parsedState <= 0xFFUL) outState = (uint8_t)parsedState;
    }
    return true;
}

static void checkAndBroadcastBatterySafeguard(uint32_t targetModuleId) {
    float battVolts = 0.0f;
    float engineRpm = 0.0f;
    if (UdsHandler::readBatteryVoltage0200(targetModuleId, battVolts, engineRpm, 350)) {
        bool isLow = (battVolts > 0.5f && battVolts < BATTERY_LOW_WARN_VOLTS);
        char telJson[220];
        snprintf(telJson, sizeof(telJson),
                 "{\"type\":\"outputs_telemetry\",\"battery_voltage\":%.2f,\"engine_speed\":%.0f,\"low_battery\":%s}",
                 battVolts, engineRpm, isLow ? "true" : "false");
        MainWebServer::broadcastWsJson(telJson);

        if (isLow) {
            char warnJson[280];
            snprintf(warnJson, sizeof(warnJson),
                     "{\"type\":\"battery_warning\",\"voltage\":%.2f,\"threshold\":%.1f,\"rpm\":%.0f,"
                     "\"message\":\"LOW BATTERY SAFEGUARD: Battery voltage dropped to %.1fV (< 11.8V) during service testing!\"}",
                     battVolts, BATTERY_LOW_WARN_VOLTS, engineRpm, battVolts);
            MainWebServer::broadcastWsJson(warnJson);
            Serial.printf("[BATTERY-SAFEGUARD] WARNING: Battery voltage dropped to %.2fV (< 11.8V) during actuator test!\n", battVolts);
        }
    }
}

static void checkAndBroadcastAbsOutputsSafeguard() {
    if (UdsHandler::readAbsInputs0200Synchronous(HD_ECU_ABS, s_web_abs_inputs, 350)) {
        MainWebServer::broadcastAbsInputs0200(s_web_abs_inputs);
        bool isMoving = (s_web_abs_inputs.front_wheel_speed > 0.0f || s_web_abs_inputs.rear_wheel_speed > 0.0f ||
                         s_web_abs_inputs.rear_right_wheel_speed > 0.0f || s_web_abs_inputs.vehicle_speed > 0.0f);
        bool isLow    = (s_web_abs_inputs.abs_ecu_module_voltage > 0.5f && s_web_abs_inputs.abs_ecu_module_voltage < BATTERY_LOW_WARN_VOLTS);
        char absTelJson[280];
        snprintf(absTelJson, sizeof(absTelJson),
                 "{\"type\":\"abs_outputs_telemetry\",\"module\":\"abs\",\"battery_voltage\":%.1f,"
                 "\"front_wheel_speed\":%.2f,\"rear_wheel_speed\":%.2f,\"vehicle_moving\":%s,\"low_battery\":%s}",
                 (double)s_web_abs_inputs.abs_ecu_module_voltage,
                 (double)s_web_abs_inputs.front_wheel_speed,
                 (double)s_web_abs_inputs.rear_wheel_speed,
                 isMoving ? "true" : "false",
                 isLow ? "true" : "false");
        MainWebServer::broadcastWsJson(absTelJson);
    }
}

void MainWebServer::processWsActionJson(const char *messageBuffer) {
    if (messageBuffer == nullptr || messageBuffer[0] == '\0') return;

    // 1A. ABS UDS Service 0x2F Actuator Control (Target CAN ID: 0x7E1):
    //     {"action":"control_abs_output", "did":"0540", "state": 1}
    if (strstr(messageBuffer, "control_abs_output") != nullptr) {
        outputs_view_active     = true;
        abs_outputs_view_active = true;
        uint16_t did   = HD_DID_ACT_ABS_PUMP_MOTOR;
        uint8_t  state = 1;
        extractJsonDidAndState(messageBuffer, did, state);
        if (did == HD_DID_ACT_FUEL_PUMP && strstr(messageBuffer, "\"did\"") == nullptr) {
            did = HD_DID_ACT_ABS_PUMP_MOTOR;
        }

        ActuatorOutputResult actuatorResult;
        bool succeeded = UdsHandler::controlAbsActuatorOutput(did, state, actuatorResult, 600);

        char responseJson[520];
        snprintf(responseJson, sizeof(responseJson),
                 "{\"type\":\"actuator_response\",\"module\":\"abs\",\"target_id\":\"0x7E1\",\"ok\":%s,"
                 "\"did\":\"%04X\",\"requested_state\":%u,\"state\":%u,\"pulse_ms\":%u,"
                 "\"got_reply\":%s,\"is_positive\":%s,\"is_negative\":%s,\"rpm_interlock\":%s,\"speed_interlock\":%s,"
                 "\"nrc\":%u,\"tx_hex\":\"%s\",\"rx_hex\":\"%s\",\"message\":\"%s\"}",
                 succeeded ? "true" : "false",
                 actuatorResult.did,
                 actuatorResult.requested_state,
                 actuatorResult.current_state,
                 actuatorResult.pulse_duration_ms,
                 actuatorResult.got_reply ? "true" : "false",
                 actuatorResult.is_positive ? "true" : "false",
                 actuatorResult.is_negative ? "true" : "false",
                 actuatorResult.rpm_interlock ? "true" : "false",
                 actuatorResult.speed_interlock ? "true" : "false",
                 actuatorResult.nrc,
                 actuatorResult.tx_frame_hex,
                 actuatorResult.rx_frame_hex,
                 actuatorResult.message);
        broadcastWsJson(responseJson);

        last_voltage_check_ms = millis();
        return;
    }

    // 1B. ABS Failsafe Cleanup: {"action":"cleanup_abs_outputs"}
    if (strstr(messageBuffer, "cleanup_abs_outputs") != nullptr) {
        abs_outputs_view_active = false;
        outputs_view_active     = false;
        UdsHandler::cleanupAbsActuatorOutputs();
        broadcastWsJson("{\"type\":\"actuator_cleanup\",\"module\":\"abs\",\"ok\":true,"
                        "\"reset_frame\":\"04 2F 05 40 00 00 00 00\",\"session_reset\":\"02 10 01 00 00 00 00 00\","
                        "\"message\":\"ABS Hydraulic Pump Motor (0x0540) returned to ECU (04 2F 05 40 00 00 00 00), Default Session (02 10 01) restored, and 02 3E 80 TesterPresent stopped.\"}");
        return;
    }

    // 1C. Enter ABS Outputs View Notification: {"action":"enter_abs_outputs"}
    if (strstr(messageBuffer, "enter_abs_outputs") != nullptr) {
        outputs_view_active     = true;
        abs_outputs_view_active = true;
        settings.active_module_id = HD_ECU_ABS;
        strncpy(settings.active_module_name, "ABS", sizeof(settings.active_module_name) - 1);
        UdsHandler::setAbsOutputsPageActive(true);
        last_voltage_check_ms = millis();
        checkAndBroadcastAbsOutputsSafeguard();
        broadcastWsJson("{\"type\":\"abs_session_active\",\"module\":\"abs\",\"target_id\":\"0x7E1\","
                        "\"extended_session\":\"02 10 03 00 00 00 00 00\",\"tester_present\":\"02 3E 80 00 00 00 00 00\"}");
        return;
    }

    // 1D. ECM UDS Service 0x2F Actuator Control: {"action":"control_output", "did":"0502", "state": 1}
    if (strstr(messageBuffer, "control_output") != nullptr) {
        outputs_view_active = true;
        uint16_t did   = HD_DID_ACT_FUEL_PUMP;
        uint8_t  state = 1;
        extractJsonDidAndState(messageBuffer, did, state);

        uint32_t targetModuleId = (settings.active_module_id != 0) ? settings.active_module_id : HD_ECU_ECM;
        if (did == HD_DID_ACT_ABS_PUMP_MOTOR || targetModuleId == HD_ECU_ABS) {
            abs_outputs_view_active = true;
            ActuatorOutputResult absActuatorResult;
            bool absSucceeded = UdsHandler::controlAbsActuatorOutput(did, state, absActuatorResult, 600);
            char absResponseJson[520];
            snprintf(absResponseJson, sizeof(absResponseJson),
                     "{\"type\":\"actuator_response\",\"module\":\"abs\",\"target_id\":\"0x7E1\",\"ok\":%s,"
                     "\"did\":\"%04X\",\"requested_state\":%u,\"state\":%u,\"pulse_ms\":%u,"
                     "\"got_reply\":%s,\"is_positive\":%s,\"is_negative\":%s,\"rpm_interlock\":%s,\"speed_interlock\":%s,"
                     "\"nrc\":%u,\"tx_hex\":\"%s\",\"rx_hex\":\"%s\",\"message\":\"%s\"}",
                     absSucceeded ? "true" : "false",
                     absActuatorResult.did,
                     absActuatorResult.requested_state,
                     absActuatorResult.current_state,
                     absActuatorResult.pulse_duration_ms,
                     absActuatorResult.got_reply ? "true" : "false",
                     absActuatorResult.is_positive ? "true" : "false",
                     absActuatorResult.is_negative ? "true" : "false",
                     absActuatorResult.rpm_interlock ? "true" : "false",
                     absActuatorResult.speed_interlock ? "true" : "false",
                     absActuatorResult.nrc,
                     absActuatorResult.tx_frame_hex,
                     absActuatorResult.rx_frame_hex,
                     absActuatorResult.message);
            broadcastWsJson(absResponseJson);
            return;
        }

        // ECM output tests moved to the dedicated engine (EcmOutputs, /api/ecm/outputs/*).
        broadcastWsJson("{\"type\":\"ecm_out\",\"event\":\"status\",\"msg\":\"Legacy control_output is no longer used for the ECM - use the new Outputs page\"}");
        return;
    }

    // 2. Failsafe Cleanup: {"action":"cleanup_outputs"}
    if (strstr(messageBuffer, "cleanup_outputs") != nullptr) {
        outputs_view_active = false;
        uint32_t targetModuleId = (settings.active_module_id != 0) ? settings.active_module_id : HD_ECU_ECM;
        if (abs_outputs_view_active || targetModuleId == HD_ECU_ABS || UdsHandler::isAbsOutputsPageActive()) {
            abs_outputs_view_active = false;
            UdsHandler::cleanupAbsActuatorOutputs();
            broadcastWsJson("{\"type\":\"actuator_cleanup\",\"module\":\"abs\",\"ok\":true,"
                            "\"reset_frame\":\"04 2F 05 40 00 00 00 00\",\"session_reset\":\"02 10 01 00 00 00 00 00\","
                            "\"message\":\"ABS Hydraulic Pump Motor (0x0540) returned to ECU (04 2F 05 40 00 00 00 00), Default Session (02 10 01) restored, and 02 3E 80 TesterPresent stopped.\"}");
            return;
        }
        EcmOutputs::exitPage("cleanup");
        SpdoOutputs::exitPage("cleanup");
        BcmOutputs::exitPage("cleanup");
        AbsOutputs::exitPage("cleanup");
        EcmSpecial::exitPage("failsafe");
        BcmSpecial::exitPage("failsafe");
        AbsSpecial::exitPage("failsafe");
        SpdoSpecial::exitPage("failsafe");
        RadSpecial::exitPage("failsafe");
        LhcSpecial::exitPage("failsafe");
        return;
    }

    // 3. Enter Outputs View Notification: {"action":"enter_outputs"}
    if (strstr(messageBuffer, "enter_outputs") != nullptr) {
        outputs_view_active = true;
        uint32_t targetModuleId = (settings.active_module_id != 0) ? settings.active_module_id : HD_ECU_ECM;
        last_voltage_check_ms = millis();
        if (targetModuleId == HD_ECU_ABS) {
            abs_outputs_view_active = true;
            UdsHandler::setAbsOutputsPageActive(true);
            checkAndBroadcastAbsOutputsSafeguard();
        }
        return;
    }

    // 4. Live Stream DID 0x0200 Start / Stop (Supports ECM 0x7E0, ABS 0x7E1, and BCM 0x7E2)
    if (strstr(messageBuffer, "start_stream") != nullptr || strstr(messageBuffer, "0200") != nullptr) {
        if (strstr(messageBuffer, "\"abs\"") != nullptr || strstr(messageBuffer, "\"ABS\"") != nullptr || strstr(messageBuffer, "7E1") != nullptr) {
            live_stream_0200_module = HD_ECU_ABS;
        } else if (strstr(messageBuffer, "\"bcm\"") != nullptr || strstr(messageBuffer, "\"BCM\"") != nullptr || strstr(messageBuffer, "7E2") != nullptr) {
            live_stream_0200_module = HD_ECU_BCM;
        } else if (strstr(messageBuffer, "\"ecm\"") != nullptr || strstr(messageBuffer, "\"ECM\"") != nullptr || strstr(messageBuffer, "7E0") != nullptr) {
            live_stream_0200_module = HD_ECU_ECM;
        } else if (strstr(messageBuffer, "7D0") != nullptr) {
            live_stream_0200_module = HD_ECU_SCM;
        } else if (strstr(messageBuffer, "7E3") != nullptr) {
            live_stream_0200_module = HD_ECU_LHC;
        } else if (strstr(messageBuffer, "7E5") != nullptr) {
            live_stream_0200_module = HD_ECU_RHC;
        } else if (strstr(messageBuffer, "7E7") != nullptr) {
            live_stream_0200_module = HD_ECU_EHCU;
        } else if (strstr(messageBuffer, "7F2") != nullptr) {
            live_stream_0200_module = HD_ECU_SPEEDO;
        } else if (settings.active_module_id != 0) {
            live_stream_0200_module = settings.active_module_id;
        }
        if (strstr(messageBuffer, "stop_stream") != nullptr) {
            MainWebServer::setLiveStream0200(false);
        } else {
            MainWebServer::setLiveStream0200(true);
        }
    } else if (strstr(messageBuffer, "stop_stream") != nullptr) {
        MainWebServer::setLiveStream0200(false);
    }
}

static void serviceWsClients() {
    for (size_t clientIndex = 0; clientIndex < MAX_WS_CLIENTS; clientIndex++) {
        bool currentlyConnected = (wsClients[clientIndex] && wsClients[clientIndex].connected());
        if (!currentlyConnected && wsClientWasConnected[clientIndex]) {
            wsClientWasConnected[clientIndex] = false;
            Serial.println("[WS] WebSocket client disconnected.");
            // Failsafe Requirement: If WebSocket drops while ABS or ECM outputs view or any actuator is active,
            // immediately execute ReturnControlToECU + Default Session (02 10 01) + Stop TesterPresent!
            if (abs_outputs_view_active || UdsHandler::isAbsOutputsPageActive()) {
                abs_outputs_view_active = false;
                outputs_view_active     = false;
                Serial.println("[WS-FAILSAFE] Connection dropped during ABS actuator testing -> Sending 04 2F 05 40 00 00 00 00 + 02 10 01 to 0x7E1 & stopping 02 3E 80!");
                UdsHandler::cleanupAbsActuatorOutputs();
            } else if (EcmOutputs::pageActive() || EcmOutputs::anyOutputActive() || BcmOutputs::pageActive() || BcmOutputs::anyOutputActive() || AbsOutputs::pageActive() || AbsOutputs::anyOutputActive() || SpdoOutputs::pageActive() || SpdoOutputs::anyOutputActive() || BcmSpecial::pageActive() || AbsSpecial::pageActive() || SpdoSpecial::pageActive() || RadSpecial::pageActive() || LhcSpecial::pageActive()) {
                outputs_view_active = false;
                uint32_t targetModuleId = (settings.active_module_id != 0) ? settings.active_module_id : HD_ECU_ECM;
                Serial.println("[WS-FAILSAFE] Connection dropped during actuator testing -> Executing 7-DID cleanup + 02 10 01!");
                EcmOutputs::exitPage("failsafe");
                BcmOutputs::exitPage("failsafe");
                AbsOutputs::exitPage("failsafe");
                SpdoOutputs::exitPage("failsafe");
                EcmSpecial::exitPage("failsafe");
                BcmSpecial::exitPage("failsafe");
                AbsSpecial::exitPage("failsafe");
                SpdoSpecial::exitPage("failsafe");
                RadSpecial::exitPage("failsafe");
                LhcSpecial::exitPage("failsafe");
            }
        }

        if (currentlyConnected) {
            wsClientWasConnected[clientIndex] = true;
            while (wsClients[clientIndex].available() >= 2) {
                uint8_t frameFirstByte = wsClients[clientIndex].read();
                uint8_t frameSecondByte = wsClients[clientIndex].read();
                uint8_t opcode = frameFirstByte & 0x0F;
                bool masked = (frameSecondByte & 0x80) != 0;
                uint64_t framePayloadLength = frameSecondByte & 0x7F;

                if (framePayloadLength == 126 && wsClients[clientIndex].available() >= 2) {
                    framePayloadLength = ((uint16_t)wsClients[clientIndex].read() << 8) | wsClients[clientIndex].read();
                }
                uint8_t maskKey[4] = {0, 0, 0, 0};
                if (masked) {
                    for (int maskByteIndex = 0; maskByteIndex < 4 && wsClients[clientIndex].available(); maskByteIndex++) {
                        maskKey[maskByteIndex] = wsClients[clientIndex].read();
                    }
                }
                char messageBuffer[224] = {0};
                for (size_t payloadByteIndex = 0; payloadByteIndex < framePayloadLength && wsClients[clientIndex].available(); payloadByteIndex++) {
                    uint8_t decodedByte = wsClients[clientIndex].read();
                    if (masked) decodedByte ^= maskKey[payloadByteIndex % 4];
                    if (payloadByteIndex < sizeof(messageBuffer) - 1) messageBuffer[payloadByteIndex] = (char)decodedByte;
                }

                if (opcode == 0x08) { // Connection Close
                    wsClients[clientIndex].stop();
                    wsClientWasConnected[clientIndex] = false;
                    if (abs_outputs_view_active || UdsHandler::isAbsOutputsPageActive()) {
                        abs_outputs_view_active = false;
                        outputs_view_active     = false;
                        Serial.println("[WS-FAILSAFE] Client closed WebSocket during ABS actuator testing -> Sending 04 2F 05 40 00 00 00 00 + 02 10 01 to 0x7E1!");
                        UdsHandler::cleanupAbsActuatorOutputs();
                    } else if (EcmOutputs::pageActive() || EcmOutputs::anyOutputActive() || BcmOutputs::pageActive() || BcmOutputs::anyOutputActive() || AbsOutputs::pageActive() || AbsOutputs::anyOutputActive() || SpdoOutputs::pageActive() || SpdoOutputs::anyOutputActive() || BcmSpecial::pageActive() || AbsSpecial::pageActive() || SpdoSpecial::pageActive() || RadSpecial::pageActive() || LhcSpecial::pageActive()) {
                        outputs_view_active = false;
                        uint32_t targetModuleId = (settings.active_module_id != 0) ? settings.active_module_id : HD_ECU_ECM;
                        Serial.println("[WS-FAILSAFE] Client closed WebSocket during actuator testing -> Executing 7-DID cleanup + 02 10 01!");
                        EcmOutputs::exitPage("failsafe");
                        BcmOutputs::exitPage("failsafe");
                        AbsOutputs::exitPage("failsafe");
                        SpdoOutputs::exitPage("failsafe");
                        EcmSpecial::exitPage("failsafe");
                        BcmSpecial::exitPage("failsafe");
                        AbsSpecial::exitPage("failsafe");
                        SpdoSpecial::exitPage("failsafe");
                        RadSpecial::exitPage("failsafe");
                        LhcSpecial::exitPage("failsafe");
                    }
                    break;
                } else if (opcode == 0x09) { // Ping -> Pong
                    uint8_t pongFrame[2] = {0x8A, 0x00};
                    wsClients[clientIndex].write(pongFrame, 2);
                } else if (opcode == 0x01) { // Text Command from Web UI
                    MainWebServer::processWsActionJson(messageBuffer);
                }
            }
        }
    }
}

// ============================================================================
// DID 0x0200 Dynamic Multi-Profile JSON Serializer & WebSocket Broadcaster
// Dynamically serializes all 41 TOTAL_PARAM_COUNT parameters from the active
// ECU generation matrix (Map_M8_0200, Map_TC_ETC_0200, Map_TC_CABLE_0200, or
// Map_XL_0200) directly bound to p.json_key.
// ============================================================================
size_t MainWebServer::serializeEcmInputs0200Json(const EcmInputs0200 &ecmInputs, char *outputJsonBuffer, size_t maxJsonLength, bool includeOkFlag) {
    if (outputJsonBuffer == nullptr || maxJsonLength < 768) return 0;

    const DynamicParamMap *activeParamMap = UdsHandler::getActiveParamMap();
    size_t writtenPosition = 0;

    int headerLength = snprintf(
        outputJsonBuffer,
        maxJsonLength,
        "{%s\"type\":\"live_data\",\"module\":\"ecm\",\"did\":\"0200\",\"ecu_family\":%d,\"data\":{",
        includeOkFlag ? "\"ok\":true," : "",
        (int)ecmInputs.ecu_family
    );
    if (headerLength <= 0 || (size_t)headerLength >= maxJsonLength) return 0;
    writtenPosition = (size_t)headerLength;

    // 1. Dynamically serialize all 41 TOTAL_PARAM_COUNT parameters bound to paramMapEntry.json_key
    for (int paramIndex = 0; paramIndex < (int)TOTAL_PARAM_COUNT; paramIndex++) {
        const DynamicParamMap &paramMapEntry = activeParamMap[paramIndex];
        int appendedLength = 0;
        const char *separator = (paramIndex == 0) ? "" : ",";

        if (ecmInputs.param_is_na[paramIndex] || (paramMapEntry.byte_offset == 0 && paramMapEntry.data_length == 0)) {
            appendedLength = snprintf(outputJsonBuffer + writtenPosition, maxJsonLength - writtenPosition, "%s\"%s\":\"N/A\"", separator, paramMapEntry.json_key);
        } else if (ecmInputs.param_is_string[paramIndex] || paramMapEntry.bit_mask != 0xFF || strcmp(paramMapEntry.json_key, "gear_position") == 0) {
            appendedLength = snprintf(outputJsonBuffer + writtenPosition, maxJsonLength - writtenPosition, "%s\"%s\":\"%s\"", separator, paramMapEntry.json_key, ecmInputs.param_strings[paramIndex]);
        } else {
            appendedLength = snprintf(outputJsonBuffer + writtenPosition, maxJsonLength - writtenPosition, "%s\"%s\":%.2f", separator, paramMapEntry.json_key, (double)ecmInputs.param_values[paramIndex]);
        }

        if (appendedLength <= 0 || writtenPosition + (size_t)appendedLength >= maxJsonLength - 64) {
            Serial.printf("[WS-0200] ERROR: JSON buffer overflow at param %d (%s)\n", paramIndex, paramMapEntry.json_key);
            return 0;
        }
        writtenPosition += (size_t)appendedLength;
    }

    // 2. Append compatibility alias keys for seamless UI binding
    int tailLength = snprintf(
        outputJsonBuffer + writtenPosition,
        maxJsonLength - writtenPosition,
        ",\"idle_speed_required\":%.1f,"
        "\"cylinder_head_temp\":%.1f,"
        "\"cylinder_head_temp_voltage\":%.2f,"
        "\"map\":%.2f,"
        "\"tps_voltage\":%.3f,"
        "\"intake_air_temp\":%.1f,"
        "\"iat_voltage\":%.2f,"
        "\"idle_air_control\":%.1f,"
        "\"injector_pw_front\":%.2f,"
        "\"injector_pw_rear\":%.2f,"
        "\"knock_front_retard\":%.2f,"
        "\"knock_rear_retard\":%.2f,"
        "\"o2_sensor_front\":%.1f,"
        "\"o2_sensor_rear\":%.1f,"
        "\"afr_front_integrator\":%.2f,"
        "\"afr_rear_integrator\":%.2f,"
        "\"engine_run_stop_switch\":\"%s\","
        "\"vehicle_theft_deterrent\":\"%s\","
        "\"vehicle_tipped\":\"%s\","
        "\"neutral_switch\":\"%s\","
        "\"clutch_switch\":\"%s\""
        "}}",
        (double)ecmInputs.idle_set_speed,
        (double)ecmInputs.engine_temperature,
        (double)ecmInputs.engine_temperature_sensor,
        (double)ecmInputs.map_kpa,
        (double)ecmInputs.throttle_position_sensor_1,
        (double)ecmInputs.intake_air_temperature,
        (double)ecmInputs.intake_air_temperature_sensor,
        (double)ecmInputs.idle_air_control_steps,
        (double)ecmInputs.injector_time_front,
        (double)ecmInputs.injector_time_rear,
        (double)ecmInputs.front_spark_knock_retard,
        (double)ecmInputs.rear_spark_knock_retard,
        (double)ecmInputs.front_oxygen_sensor,
        (double)ecmInputs.rear_oxygen_sensor,
        (double)ecmInputs.front_closed_loop_integrator,
        (double)ecmInputs.rear_closed_loop_integrator,
        ecmInputs.run_mode_flag[0] ? ecmInputs.run_mode_flag : "Off",
        ecmInputs.tssm_security_state[0] ? ecmInputs.tssm_security_state : "Disarmed",
        ecmInputs.bank_angle_sensor_mode[0] ? ecmInputs.bank_angle_sensor_mode : "Run Mode",
        ecmInputs.neutral_switch_input[0] ? ecmInputs.neutral_switch_input : "In Gear",
        ecmInputs.clutch_lever_switch_state[0] ? ecmInputs.clutch_lever_switch_state : "Released"
    );

    if (tailLength <= 0 || writtenPosition + (size_t)tailLength >= maxJsonLength) {
        Serial.printf("[WS-0200] ERROR: JSON tail buffer overflow\n");
        return 0;
    }
    writtenPosition += (size_t)tailLength;
    return writtenPosition;
}

void MainWebServer::broadcastEcmInputs0200(const EcmInputs0200 &ecmInputs) {
    size_t serializedLength = serializeEcmInputs0200Json(ecmInputs, s_ws_ecm_json, sizeof(s_ws_ecm_json), false);
    if (serializedLength == 0) return;
    broadcastWsJson(s_ws_ecm_json, serializedLength);
}

// ============================================================================
// ABS (0x7E1) DID 0x0200 JSON Serializer & WebSocket Broadcaster
// Format: {"type":"live_data","module":"abs","did":"0200","data":{"front_wheel_speed":0.0,...}}
// ============================================================================
size_t MainWebServer::serializeAbsInputs0200Json(const AbsInputs0200 &absInputs, char *outputJsonBuffer, size_t maxJsonLength, bool includeOkFlag) {
    if (outputJsonBuffer == nullptr || maxJsonLength < 256) return 0;

    // Only the parameters that exist in the active ABS family (and fit in the received payload) are emitted;
    // the UI builds its rows from the keys present in "data".
    size_t writtenPosition = (size_t)snprintf(
        outputJsonBuffer, maxJsonLength,
        "{%s\"type\":\"live_data\",\"module\":\"abs\",\"did\":\"0200\",\"abs_family\":\"%s\",\"data\":{",
        includeOkFlag ? "\"ok\":true," : "",
        ABS_FAMILY_MAPS[absInputs.family < ABS_FAMILY_COUNT ? absInputs.family : (uint8_t)ABS_FAMILY_DEFAULT].name);
    if (writtenPosition >= maxJsonLength) return 0;

    bool isFirstField = true;
    for (uint8_t valueIndex = 0; valueIndex < ABSV_COUNT; valueIndex++) {
        if (!(absInputs.value_present & (1U << valueIndex))) continue;
        int appendedLength = snprintf(outputJsonBuffer + writtenPosition, maxJsonLength - writtenPosition, "%s\"%s\":%.3f", isFirstField ? "" : ",", ABS_VALUE_KEYS[valueIndex], (double)absInputs.values[valueIndex]);
        if (appendedLength < 0 || (size_t)appendedLength >= maxJsonLength - writtenPosition) { Serial.println("[WS-ABS-0200] ERROR: JSON buffer overflow"); return 0; }
        writtenPosition += (size_t)appendedLength; isFirstField = false;
    }
    for (uint8_t stateIndex = 0; stateIndex < ABSS_COUNT; stateIndex++) {
        if (!(absInputs.state_present & (1UL << stateIndex))) continue;
        const bool isStateOn = (absInputs.state_on & (1UL << stateIndex)) != 0;
        const char *stateText = ABS_STATE_YESNO[stateIndex] ? (isStateOn ? "Yes" : "No") : (isStateOn ? "On" : "Off");
        int appendedLength = snprintf(outputJsonBuffer + writtenPosition, maxJsonLength - writtenPosition, "%s\"%s\":\"%s\"", isFirstField ? "" : ",", ABS_STATE_KEYS[stateIndex], stateText);
        if (appendedLength < 0 || (size_t)appendedLength >= maxJsonLength - writtenPosition) { Serial.println("[WS-ABS-0200] ERROR: JSON buffer overflow"); return 0; }
        writtenPosition += (size_t)appendedLength; isFirstField = false;
    }
    int closingLength = snprintf(outputJsonBuffer + writtenPosition, maxJsonLength - writtenPosition, "}}");
    if (closingLength < 0 || (size_t)closingLength >= maxJsonLength - writtenPosition) return 0;
    return writtenPosition + (size_t)closingLength;
}

void MainWebServer::broadcastAbsInputs0200(const AbsInputs0200 &absInputs) {
    size_t serializedLength = serializeAbsInputs0200Json(absInputs, s_ws_abs_json, sizeof(s_ws_abs_json), false);
    if (serializedLength == 0) return;
    broadcastWsJson(s_ws_abs_json, serializedLength);
}

// ============================================================================
// BCM (0x7E2) DID 0x0200 JSON Serializer & WebSocket Broadcaster
// Format: {"type":"live_data","module":"bcm","did":"0200","data":{"system_voltage":12.6,...}}
// ============================================================================
size_t MainWebServer::serializeBcmInputs0200Json(const BcmInputs0200 &bcmInputs, char *outputJsonBuffer, size_t maxJsonLength, bool includeOkFlag) {
    if (outputJsonBuffer == nullptr || maxJsonLength < 256) return 0;

    // Only parameters that exist in the active BCM family (and fit in the received payload) are emitted.
    size_t writtenPosition = (size_t)snprintf(
        outputJsonBuffer, maxJsonLength,
        "{%s\"type\":\"live_data\",\"module\":\"bcm\",\"did\":\"0200\",\"bcm_family\":\"%s\",\"data\":{",
        includeOkFlag ? "\"ok\":true," : "",
        BCM_FAMILY_MAPS[bcmInputs.family < BCM_FAMILY_COUNT ? bcmInputs.family : (uint8_t)BCM_FAMILY_DEFAULT].name);
    if (writtenPosition >= maxJsonLength) return 0;

    bool isFirstField = true;
    if (bcmInputs.voltage_present) {
        int appendedLength = snprintf(outputJsonBuffer + writtenPosition, maxJsonLength - writtenPosition, "\"system_voltage\":%.3f", (double)bcmInputs.system_voltage);
        if (appendedLength < 0 || (size_t)appendedLength >= maxJsonLength - writtenPosition) return 0;
        writtenPosition += (size_t)appendedLength; isFirstField = false;
    }
    for (uint8_t stateIndex = 0; stateIndex < BCMS_COUNT; stateIndex++) {
        if (!(bcmInputs.state_present & (1ULL << stateIndex))) continue;
        const bool isStateOn = (bcmInputs.state_on & (1ULL << stateIndex)) != 0;
        int appendedLength = snprintf(outputJsonBuffer + writtenPosition, maxJsonLength - writtenPosition, "%s\"%s\":\"%s\"", isFirstField ? "" : ",", BCM_STATE_KEYS[stateIndex], BCM_STATE_TEXT[stateIndex][isStateOn ? 0 : 1]);
        if (appendedLength < 0 || (size_t)appendedLength >= maxJsonLength - writtenPosition) { Serial.println("[WS-BCM-0200] ERROR: JSON buffer overflow"); return 0; }
        writtenPosition += (size_t)appendedLength; isFirstField = false;
    }
    int closingLength = snprintf(outputJsonBuffer + writtenPosition, maxJsonLength - writtenPosition, "}}");
    if (closingLength < 0 || (size_t)closingLength >= maxJsonLength - writtenPosition) return 0;
    return writtenPosition + (size_t)closingLength;
}

size_t MainWebServer::serializeGenInputs0200Json(const GenInputs0200 &genInputs, char *outputJsonBuffer, size_t maxJsonLength, bool includeOkFlag) {
    if (outputJsonBuffer == nullptr || maxJsonLength < 256) return 0;
    const GenModuleDef* genModuleDef = findGenModule(genInputs.module_id);
    if (!genModuleDef) return 0;
    char lowercaseModuleName[8]; size_t nameCharIndex = 0;
    for (; genModuleDef->name[nameCharIndex] && nameCharIndex < sizeof(lowercaseModuleName) - 1; nameCharIndex++) lowercaseModuleName[nameCharIndex] = (char)tolower((unsigned char)genModuleDef->name[nameCharIndex]);
    lowercaseModuleName[nameCharIndex] = 0;
    size_t writtenPosition = (size_t)snprintf(outputJsonBuffer, maxJsonLength,
        "{%s\"type\":\"live_data\",\"module\":\"%s\",\"did\":\"0200\",\"gen_layout\":\"%s\",\"data\":{",
        includeOkFlag ? "\"ok\":true," : "", lowercaseModuleName,
        genModuleDef->layouts[genInputs.layout < genModuleDef->layout_count ? genInputs.layout : genModuleDef->default_layout].name);
    if (writtenPosition >= maxJsonLength) return 0;
    for (uint8_t itemIndex = 0; itemIndex < genInputs.count; itemIndex++) {
        int appendedLength = (genInputs.kind[itemIndex] == GEN_FLAG)
            ? snprintf(outputJsonBuffer + writtenPosition, maxJsonLength - writtenPosition, "%s\"%s\":%d", itemIndex ? "," : "", GEN_KEYS[genInputs.key[itemIndex]], genInputs.value[itemIndex] > 0.5f ? 1 : 0)
            : snprintf(outputJsonBuffer + writtenPosition, maxJsonLength - writtenPosition, "%s\"%s\":%.3f", itemIndex ? "," : "", GEN_KEYS[genInputs.key[itemIndex]], (double)genInputs.value[itemIndex]);
        if (appendedLength < 0 || (size_t)appendedLength >= maxJsonLength - writtenPosition) { Serial.println("[WS-GEN-0200] ERROR: JSON buffer overflow"); return 0; }
        writtenPosition += (size_t)appendedLength;
    }
    int closingLength = snprintf(outputJsonBuffer + writtenPosition, maxJsonLength - writtenPosition, "}}");
    if (closingLength < 0 || (size_t)closingLength >= maxJsonLength - writtenPosition) return 0;
    return writtenPosition + (size_t)closingLength;
}

void MainWebServer::broadcastGenInputs0200(const GenInputs0200 &genInputs) {
    static char genJsonBuffer[2048];
    size_t serializedLength = serializeGenInputs0200Json(genInputs, genJsonBuffer, sizeof(genJsonBuffer), false);
    if (serializedLength == 0) return;
    broadcastWsJson(genJsonBuffer, serializedLength);
}

void MainWebServer::broadcastBcmInputs0200(const BcmInputs0200 &bcmInputs) {
    size_t serializedLength = serializeBcmInputs0200Json(bcmInputs, s_ws_bcm_json, sizeof(s_ws_bcm_json), false);
    if (serializedLength == 0) return;
    broadcastWsJson(s_ws_bcm_json, serializedLength);
}

void MainWebServer::setLiveStream0200(bool enableStreaming) {
    live_stream_0200_enabled = enableStreaming;
    if (enableStreaming) {
        // When entering "Read Module Inputs" (DID 0x0200 live streaming), pause standard
        // TesterPresent (0x3E 00) because the rapid periodic ReadDataByIdentifier (0x22 0x02 0x00)
        // frames naturally act as a functional keep-alive for the ECU.
        outputs_view_active     = false;
        abs_outputs_view_active = false;
        UdsHandler::setAbsOutputsPageActive(false);
        uint32_t targetModuleId = (live_stream_0200_module != 0)
            ? live_stream_0200_module
            : ((settings.active_module_id != 0) ? settings.active_module_id : HD_ECU_ECM);
        UdsHandler::setTesterPresent(false, targetModuleId);
        // Set timestamp to millis() so the initial HTTP /api/uds/inputs query does not
        // collide back-to-back with the 200ms WebSocket poll in the same loop iteration.
        last_live_stream_0200_ms = millis();
        Serial.printf("[WS-0200] Live telemetry stream ENABLED for %s (0x%03X) DID 0x0200 (Standard 0x3E paused; 0x22 0200 acts as functional keep-alive).\n",
                      lookupModuleName(targetModuleId),
                      targetModuleId);
    } else {
        // Immediately resume standard TesterPresent (0x3E 00) every 2000ms whenever DID 0x0200
        // streaming stops or the user navigates away from the Inputs menu.
        uint32_t resumeTargetModuleId = (settings.active_module_id != 0)
            ? settings.active_module_id
            : ((live_stream_0200_module != 0) ? live_stream_0200_module : HD_ECU_ECM);
        if (resumeTargetModuleId >= HD_MIN_DIAG_ID && resumeTargetModuleId <= HD_MAX_DIAG_ID) {
            UdsHandler::setTesterPresent(true, resumeTargetModuleId);
        }
        Serial.printf("[WS-0200] Live telemetry stream STOPPED for DID 0x0200 -> Resumed standard TesterPresent (0x3E 00 every 2000ms) on 0x%03X.\n",
                      resumeTargetModuleId);
    }
}

void MainWebServer::handleClient() {
    server.handleClient();
    serviceWsClients();

    // 1. Service non-blocking asynchronous pulse timers for UDS 0x2F actuators
    //    (e.g., 1000ms auto-reset for ABS Pump Motor 0x0540 on 0x7E1, 2000ms for Fuel Pump 0x0502 on 0x7E0)
    uint16_t resetDid = 0;
    uint32_t resetTargetId = HD_ECU_ECM;
    char resetTxHex[32] = {0};
    if (UdsHandler::serviceActuatorTimers(resetDid, resetTargetId, resetTxHex, sizeof(resetTxHex))) {
        char autoResetJson[300];
        snprintf(autoResetJson, sizeof(autoResetJson),
                 "{\"type\":\"actuator_response\",\"module\":\"%s\",\"target_id\":\"0x%03X\",\"ok\":true,"
                 "\"did\":\"%04X\",\"state\":0,\"auto_reset\":true,\"is_positive\":true,\"tx_hex\":\"%s\","
                 "\"message\":\"Pulse complete -> Returned control to ECU [%s].\"}",
                 (resetTargetId == HD_ECU_ABS) ? "abs" : "ecm",
                 resetTargetId,
                 resetDid,
                 resetTxHex,
                 resetTxHex);
        broadcastWsJson(autoResetJson);
    }

    // 1B. ECM output-test engine: keep-alive, pulse timers, engine-speed interlock, status push
    EcmOutputs::loop();
    BcmOutputs::loop();
    AbsOutputs::loop();
    SpdoOutputs::loop();
    EcmSpecial::loop();
    BcmSpecial::loop();
    AbsSpecial::loop();
    SpdoSpecial::loop();
    RadSpecial::loop();
    {
        static char ecmOutStatus[1280];
        if (EcmOutputs::takeStatusUpdate(ecmOutStatus, sizeof(ecmOutStatus))) {
            broadcastWsJson(ecmOutStatus);
        }
        if (BcmOutputs::takeStatusUpdate(ecmOutStatus, sizeof(ecmOutStatus))) {
            broadcastWsJson(ecmOutStatus);
        }
        if (AbsOutputs::takeStatusUpdate(ecmOutStatus, sizeof(ecmOutStatus))) {
            broadcastWsJson(ecmOutStatus);
        }
        if (SpdoOutputs::takeStatusUpdate(ecmOutStatus, sizeof(ecmOutStatus))) {
            broadcastWsJson(ecmOutStatus);
        }
    }

    // 2. Voltage & Wheel Speed Interlock Monitor (Battery & ABS Safeguard):
    //    CRITICAL BUS ARBITRATION GATE: Only execute when !live_stream_0200_enabled so the
    //    1200ms background safeguard never collides with or blocks the 200ms DID 0x0200 WebSocket stream!
    if (!live_stream_0200_enabled &&
        (UdsHandler::isAnyActuatorActive() || outputs_view_active || abs_outputs_view_active)) {
        uint32_t now = millis();
        if (now - last_voltage_check_ms >= 1200) {
            last_voltage_check_ms = now;
            uint32_t targetModuleId = (settings.active_module_id != 0) ? settings.active_module_id : HD_ECU_ECM;
            if (abs_outputs_view_active || targetModuleId == HD_ECU_ABS) {
                checkAndBroadcastAbsOutputsSafeguard();
            } else {
                checkAndBroadcastBatterySafeguard(targetModuleId);
            }
        }
    }

    // 3. Live DID 0x0200 telemetry: polling happens in liveStreamPollTask(); this loop only
    //    tells the poller whether any WebSocket client is connected and forwards finished frames.
    {
        bool anyWsConnected = false;
        for (size_t clientIndex = 0; clientIndex < MAX_WS_CLIENTS; clientIndex++) {
            if (wsClients[clientIndex] && wsClients[clientIndex].connected()) {
                anyWsConnected = true;
                break;
            }
        }
        s_any_ws_client = anyWsConnected;

        if (anyWsConnected && s_stream_mutex != nullptr && s_stream_seq != s_stream_sent_seq) {
            if (xSemaphoreTake(s_stream_mutex, 0) == pdTRUE) {
                s_stream_sent_seq = s_stream_seq;
                if (s_stream_len > 0) {
                    broadcastWsJson(s_stream_json, s_stream_len);
                }
                xSemaphoreGive(s_stream_mutex);
            }
        }
    }
}

// ============================================================================
// Core 1 background task: periodic UDS DID 0x0200 read (every 500 ms / 2 Hz) -> JSON frame.
// Runs at priority 2 (above the Arduino loop task) but spends nearly all its time blocked
// waiting for ECU replies, so the HTTP/WebSocket loop stays responsive. All UDS access is
// serialized with the rest of the firmware by the UdsHandler bus lock.
// ============================================================================
static void liveStreamPollTask(void *taskParam) {
    (void)taskParam;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!live_stream_0200_enabled || !s_any_ws_client) continue;

        uint32_t now = millis();
        if ((uint32_t)(now - last_live_stream_0200_ms) < 500) continue;   // 500 ms / 2 Hz live read
        last_live_stream_0200_ms = now;

        uint32_t targetModuleId = (live_stream_0200_module != 0)
            ? live_stream_0200_module
            : ((settings.active_module_id != 0) ? settings.active_module_id : HD_ECU_ECM);

        // 1) Read from the ECU (blocking, but only this task waits).
        bool readSucceeded = false;
        if (targetModuleId == HD_ECU_ABS) {
            readSucceeded = UdsHandler::readAbsInputs0200Synchronous(targetModuleId, s_poll_abs, 350);
        } else if (targetModuleId == HD_ECU_BCM) {
            readSucceeded = UdsHandler::readBcmInputs0200Synchronous(targetModuleId, s_poll_bcm, 350);
        } else if (targetModuleId == HD_ECU_ECM) {
            readSucceeded = UdsHandler::readEcmInputs0200Synchronous(targetModuleId, s_poll_ecm, 380);
        } else if (findGenModule(targetModuleId)) {
            readSucceeded = UdsHandler::readGenInputs0200Synchronous(targetModuleId, s_poll_gen, 380);
        }

        // 2) Serialize straight into the shared frame buffer (sub-millisecond). The stream may have been
        //    stopped while the read was in flight -> drop the stale frame.
        if (readSucceeded && live_stream_0200_enabled && s_stream_mutex != nullptr &&
            xSemaphoreTake(s_stream_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            size_t serializedLength = 0;
            if (targetModuleId == HD_ECU_ABS) {
                serializedLength = MainWebServer::serializeAbsInputs0200Json(s_poll_abs, s_stream_json, sizeof(s_stream_json), false);
            } else if (targetModuleId == HD_ECU_BCM) {
                serializedLength = MainWebServer::serializeBcmInputs0200Json(s_poll_bcm, s_stream_json, sizeof(s_stream_json), false);
            } else if (findGenModule(targetModuleId)) {
                serializedLength = MainWebServer::serializeGenInputs0200Json(s_poll_gen, s_stream_json, sizeof(s_stream_json), false);
            } else {
                serializedLength = MainWebServer::serializeEcmInputs0200Json(s_poll_ecm, s_stream_json, sizeof(s_stream_json), false);
            }
            s_stream_len = serializedLength;
            if (serializedLength > 0) s_stream_seq = s_stream_seq + 1;
            xSemaphoreGive(s_stream_mutex);
        }
    }
}

// ============================================================================
// Initialize Web Server & Register Diagnostic Endpoints
// ============================================================================
// Shared static response buffers for the HTTP API. The web server (WebServer::handleClient()) processes one
// request at a time on a single task, so these are safe to reuse across routes - doing so saves several KB of
// DRAM versus a separate static buffer per endpoint (each "static" local otherwise reserves .bss for the life
// of the program, not just while in use).
static char g_apiBufBig[9216];     // ecm/bcm/rad/spdo special catalog & profile responses (largest JSON; the radio speaker-configuration list alone is ~4 KB)
static char g_apiBufMed[3584];     // ecm/outputs + bcm/outputs catalog & profile responses, ecu/info/plan
static char g_apiBufSmall[1280];   // every other single-item JSON response (read/run/restore/raw/status/vin/item)

// ----------------------------------------------------------------------------
// BCM key-FOB helpers (shared by /api/bcm/fob_status and /api/bcm/fob_write).
// The CAN link here is a WiFi UDS-CAN bridge, so single transactions can time
// out or drop. These helpers lengthen timeouts and retry, and reads are only
// trusted once two attempts agree.
// ----------------------------------------------------------------------------

// Append the DID payload (everything past SID + 2 DID bytes) of a positive reply as uppercase hex.
static void bcmPayloadHex(const UdsRawResult &rr, char *out, size_t outLen) {
    out[0] = 0;
    if (!rr.got_reply || !rr.positive || rr.len <= 3) return;
    size_t q = 0;
    for (uint8_t i = 3; i < rr.len && q + 2 < outLen; i++) q += snprintf(out + q, outLen - q, "%02X", rr.data[i]);
}

// Unlock the BCM (Security Access, BCM algorithm). A 0x0000 seed means "already unlocked / no key" and the
// key is NOT sent. Retries the seed request up to `tries` times for the flaky bridge. Returns true if unlocked.
static bool bcmUnlockRetry(int tries) {
    for (int attempt = 0; attempt < tries; attempt++) {
        UdsRawResult r;
        uint8_t seedReq[2] = {0x27, 0x01};
        if (UdsHandler::rawTransaction(HD_ECU_BCM, seedReq, 2, r, 600) && r.positive && r.len >= 2) {
            uint8_t sl = (uint8_t)(r.len - 2);
            bool seedZero = true;
            for (uint8_t i = 0; i < sl; i++) if (r.data[2 + i]) seedZero = false;
            if (seedZero) return true;                                   // no unlock required
            uint8_t key[16], kl = 0;
            if (bcmComputeSecurityKey(&r.data[2], sl, key, &kl) && kl) {
                uint8_t kr2[18] = {0x27, 0x02}; memcpy(kr2 + 2, key, kl);
                UdsRawResult kr; UdsHandler::rawTransaction(HD_ECU_BCM, kr2, (uint8_t)(2 + kl), kr, 600);
                if (kr.got_reply && kr.positive) return true;
            }
        }
        yield();   // feed the watchdog between attempts
    }
    return false;
}

// Read a DID up to 6 times and only accept a value once ANY two reads agree (returns early then). Uses the
// same per-read budget that "Read ECU information" uses and never errors with (readDid: 500 ms + a short pause
// between tries) - that short-timeout-but-retry-often pattern is what makes it reliable over the WiFi bridge.
// If no two of the six agree, falls back to the first non-empty read. *confirmed is true only on a 2-of-N match.
static void bcmReadDidConfirmed(uint8_t didHi, uint8_t didLo, char *out, size_t outLen, bool *confirmed) {
    out[0] = 0; *confirmed = false;
    char samples[6][40]; int ns = 0;
    for (int t = 0; t < 6; t++) {
        UdsRawResult rr;
        uint8_t q[3] = {0x22, didHi, didLo};
        if (UdsHandler::rawTransaction(HD_ECU_BCM, q, 3, rr, 500)) bcmPayloadHex(rr, samples[ns], sizeof(samples[ns]));
        else samples[ns][0] = 0;
        for (int p = 0; p < ns; p++) {
            if (samples[ns][0] && strcmp(samples[ns], samples[p]) == 0) {   // two reads agree -> trust it
                strncpy(out, samples[ns], outLen - 1); out[outLen - 1] = 0; *confirmed = true; return;
            }
        }
        ns++;
        delay(60);   // let the bridge settle between tries (and feed the watchdog), like readDid()
    }
    for (int p = 0; p < ns; p++) if (samples[p][0]) { strncpy(out, samples[p], outLen - 1); out[outLen - 1] = 0; return; }
}

// Parse a hex string (no separators) into bytes. Returns the byte count, 0 on malformed/odd-length input.
static uint8_t bcmHexToBytes(const char *hex, uint8_t *out, uint8_t maxBytes) {
    size_t n = strlen(hex);
    if (n == 0 || (n & 1) || (n / 2) > maxBytes) return 0;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; i += 2) {
        int hiN = nib(hex[i]), loN = nib(hex[i + 1]);
        if (hiN < 0 || loN < 0) return 0;
        out[i / 2] = (uint8_t)((hiN << 4) | loN);
    }
    return (uint8_t)(n / 2);
}

// ----------------------------------------------------------------------------
// Hand Control (LHC 0x7E3 / RHC 0x7E5) "Cruise / Push-to-Talk side" special function now lives in its own
// module, same shape as the other ECUs - see LhcSpecial (src/modules/lhc/lhc_special.h/.cpp). The
// /api/lhc/special/* handlers below just delegate to it.

void MainWebServer::init() {
    const char *headerKeys[] = {"Upgrade", "Connection", "Sec-WebSocket-Key", "If-None-Match"};
    server.collectHeaders(headerKeys, 4);

    // ------------------------------------------------------------------------
    // 0. GET /ws - RFC 6455 WebSocket Upgrade Endpoint for Live Telemetry
    // ------------------------------------------------------------------------
    server.on("/ws", HTTP_GET, []() {
        if (!server.hasHeader("Sec-WebSocket-Key")) {
            server.send(400, "text/plain", "WebSocket Upgrade Required");
            return;
        }
        String wsKey = server.header("Sec-WebSocket-Key");
        wsKey.trim();
        String concat = wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        uint8_t sha1Hash[20];
        computeRfc6455Sha1((const uint8_t *)concat.c_str(), concat.length(), sha1Hash);

        char acceptBase64[40] = {0};
        encodeRfc6455Base64(sha1Hash, sizeof(sha1Hash), acceptBase64, sizeof(acceptBase64));

        WiFiClient client = server.client();
        if (client) {
            client.setNoDelay(true);
            client.print(
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + String(acceptBase64) + "\r\n\r\n"
            );

            bool stored = false;
            for (size_t i = 0; i < MAX_WS_CLIENTS; i++) {
                if (!wsClients[i] || !wsClients[i].connected()) {
                    wsClients[i] = client;
                    wsClientWasConnected[i] = true;
                    stored = true;
                    break;
                }
            }
            if (!stored) {
                wsClients[0].stop();
                wsClients[0] = client;
                wsClientWasConnected[0] = true;
            }
            Serial.println("[WS] Client connected to /ws telemetry stream.");
        }
    });

    // ------------------------------------------------------------------------
    // 1. Static Web UI Assets (Chunked zero-heap PROGMEM streaming)
    // ------------------------------------------------------------------------
    auto handleRootView = []() {
        Serial.println("[WEB] GET / -> Serving index.html (Resetting active ECU session)");
        if (abs_outputs_view_active || UdsHandler::isAbsOutputsPageActive()) {
            abs_outputs_view_active = false;
            outputs_view_active     = false;
            UdsHandler::cleanupAbsActuatorOutputs();
        } else if (EcmOutputs::pageActive() || EcmOutputs::anyOutputActive() || BcmOutputs::pageActive() || BcmOutputs::anyOutputActive() || AbsOutputs::pageActive() || AbsOutputs::anyOutputActive() || SpdoOutputs::pageActive() || SpdoOutputs::anyOutputActive() || BcmSpecial::pageActive() || AbsSpecial::pageActive() || SpdoSpecial::pageActive() || RadSpecial::pageActive() || LhcSpecial::pageActive()) {
            outputs_view_active = false;
            EcmOutputs::exitPage("failsafe");
            BcmOutputs::exitPage("failsafe");
            AbsOutputs::exitPage("failsafe");
            SpdoOutputs::exitPage("failsafe");
            EcmSpecial::exitPage("failsafe");
            BcmSpecial::exitPage("failsafe");
            AbsSpecial::exitPage("failsafe");
            SpdoSpecial::exitPage("failsafe");
            RadSpecial::exitPage("failsafe");
            LhcSpecial::exitPage("failsafe");
        }
        MainWebServer::setLiveStream0200(false);
        UdsHandler::setTesterPresent(false, 0);
        UdsHandler::resetEcuFamilyLatch();
        settings.active_module_id = 0;
        settings.active_module_name[0] = '\0';
        sendProgmemAsset("text/html", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
    };
    server.on("/", HTTP_GET, handleRootView);
    server.on("/index.html", HTTP_GET, handleRootView);
    server.on("/style.css", HTTP_GET, []() {
        sendProgmemAsset("text/css", STYLE_CSS_GZ, STYLE_CSS_GZ_LEN);
    });
    server.on("/main.js", HTTP_GET, []() {
        sendProgmemAsset("application/javascript", MAIN_JS_GZ, MAIN_JS_GZ_LEN);
    });

    // ------------------------------------------------------------------------
    // 2. GET /api/status - Device & HD Diagnostic Session Telemetry
    // ------------------------------------------------------------------------
    server.on("/api/status", HTTP_GET, []() {
        SystemMetrics snap;
        if (xSemaphoreTake(metricsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            snap = metrics;
            xSemaphoreGive(metricsMutex);
        } else {
            snap = metrics;
        }

        const char *busStateStr = "RUNNING";
        if (snap.twai_state == 0) busStateStr = "STOPPED";
        else if (snap.twai_state == 2) busStateStr = "BUS-OFF";
        else if (snap.twai_state == 3) busStateStr = "RECOVERING";

        String ipStr = settings.wifi_ap_mode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
        int32_t rssi = settings.wifi_ap_mode ? -42 : WiFi.RSSI();

        char activeIdHex[16] = "NONE";
        if (settings.active_module_id != 0) {
            snprintf(activeIdHex, sizeof(activeIdHex), "0x%03X", settings.active_module_id);
        }

        const char *canChipStr = (settings.can_chip_type == CAN_CHIP_TJA1043) ? "tja1043" : "plain";

        char json[960];
        snprintf(
            json,
            sizeof(json),
            "{\"app_name\":\"%s\",\"node_name\":\"%s\",\"copyright\":\"%s\",\"fw_version\":\"%s\",\"fw_build\":\"%s\","
            "\"chip\":\"%s\",\"rev\":%d,\"cpu_mhz\":%u,\"uptime\":%lu,\"free_heap\":%u,\"heap\":%u,"
            "\"flash\":%u,\"sketch_size\":%u,\"ota_slot_size\":%u,\"mac\":\"%s\","
            "\"wifi_mode\":\"%s\",\"wifi_ssid\":\"%s\",\"nvs_ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"can_bitrate\":%u,\"bus_state\":\"%s\","
            "\"active_module_id\":\"%s\",\"active_module_name\":\"%s\",\"tester_present\":%s,"
            "\"can_rx\":%u,\"can_tx\":%u,\"can_err\":%u,\"uds_queries\":%u,\"uds_replies\":%u,\"safeboot_gpio\":%d,"
            "\"can_chip\":\"%s\",\"can_tx_pin\":%u,\"can_rx_pin\":%u,"
            "\"tja_stb_pin\":%u,\"tja_en_pin\":%u,\"tja_err_pin\":%u,\"tja_led_pin\":%u}",
            APP_NAME_STR,
            settings.node_name,
            APP_COPYRIGHT_STR,
            FIRMWARE_VERSION_STR,
            FIRMWARE_BUILD_NUM_STR,
            ESP.getChipModel(),
            ESP.getChipRevision(),
            ESP.getCpuFreqMHz(),
            millis() / 1000UL,
            ESP.getFreeHeap(),
            ESP.getFreeHeap(),
            ESP.getFlashChipSize(),
            ESP.getSketchSize(),
            OTA_PARTITION_SIZE_BYTES,
            WiFi.macAddress().c_str(),
            settings.wifi_ap_mode ? "AP" : "STA",
            settings.wifi_ap_mode ? settings.ap_ssid : settings.wifi_ssid,
            settings.wifi_ssid,
            ipStr.c_str(),
            (int)rssi,
            settings.can_bitrate / 1000U,
            busStateStr,
            activeIdHex,
            settings.active_module_name,
            UdsHandler::isTesterPresentEnabled() ? "true" : "false",
            snap.can_rx_count,
            snap.can_tx_count,
            snap.can_err_count,
            snap.uds_queries_count,
            snap.uds_replies_count,
            (int)SAFEBOOT_BUTTON_PIN,
            canChipStr,
            (unsigned)settings.can_tx_pin,
            (unsigned)settings.can_rx_pin,
            (unsigned)settings.tja_stb_pin,
            (unsigned)settings.tja_en_pin,
            (unsigned)settings.tja_err_pin,
            (unsigned)settings.tja_led_pin
        );
        server.send(200, "application/json", json);
    });

    // ------------------------------------------------------------------------
    // 3. POST /api/module/select (and /api/uds/module/select)
    //    Select HD ECU & Start TesterPresent Keepalive (0x3E 0x00)
    // ------------------------------------------------------------------------
    auto handleModuleSelect = []() {
        uint32_t moduleId = 0x7E0;
        if (server.hasArg("id")) {
            String moduleIdArgText = server.arg("id");
            moduleIdArgText.trim();
            if (moduleIdArgText.startsWith("0x") || moduleIdArgText.startsWith("0X")) {
                moduleId = (uint32_t)strtoul(moduleIdArgText.c_str(), nullptr, 16);
            } else {
                moduleId = (uint32_t)moduleIdArgText.toInt();
            }
        }

        const char* moduleName = lookupModuleName(moduleId);
        if (settings.active_module_id != moduleId) {
            UdsHandler::resetEcuFamilyLatch();   // different ECU -> re-identify calibration/family
        }
        settings.active_module_id = moduleId;
        strncpy(settings.active_module_name, moduleName, sizeof(settings.active_module_name) - 1);
        settings.active_module_name[sizeof(settings.active_module_name) - 1] = '\0';

        Serial.printf("[WEB] POST /api/uds/module/select -> Active ECU: 0x%03X (%s), Response ID: 0x%03X\n",
                      moduleId, strlen(moduleName) ? moduleName : "CUSTOM", moduleId + 8);

        // Start 2-second TesterPresent keepalive on Core 0
        UdsHandler::setTesterPresent(true, moduleId);

        char json[256];
        snprintf(
            json,
            sizeof(json),
            "{\"ok\":true,\"id\":\"0x%03X\",\"name\":\"%s\",\"response_id\":\"0x%03X\",\"tester_present\":true}",
            moduleId,
            moduleName,
            moduleId + 8
        );
        server.send(200, "application/json", json);
    };

    server.on("/api/module/select", HTTP_POST, handleModuleSelect);
    server.on("/api/uds/module/select", HTTP_POST, handleModuleSelect);

    // ------------------------------------------------------------------------
    // 4. POST /api/module/exit (and /api/uds/module/exit) - Exit Module
    // ------------------------------------------------------------------------
    auto handleModuleExit = []() {
        if (settings.active_module_id != 0) {
            Serial.printf("[WEB] POST /api/uds/module/exit -> Exiting ECU 0x%03X (%s)\n",
                          settings.active_module_id, settings.active_module_name);
        }
        if (abs_outputs_view_active || UdsHandler::isAbsOutputsPageActive()) {
            abs_outputs_view_active = false;
            outputs_view_active     = false;
            UdsHandler::cleanupAbsActuatorOutputs();
        } else if (EcmOutputs::pageActive() || EcmOutputs::anyOutputActive() || BcmOutputs::pageActive() || BcmOutputs::anyOutputActive() || AbsOutputs::pageActive() || AbsOutputs::anyOutputActive() || SpdoOutputs::pageActive() || SpdoOutputs::anyOutputActive() || BcmSpecial::pageActive() || AbsSpecial::pageActive() || SpdoSpecial::pageActive() || RadSpecial::pageActive() || LhcSpecial::pageActive()) {
            outputs_view_active = false;
            EcmOutputs::exitPage("failsafe");
            BcmOutputs::exitPage("failsafe");
            AbsOutputs::exitPage("failsafe");
            SpdoOutputs::exitPage("failsafe");
            EcmSpecial::exitPage("failsafe");
            BcmSpecial::exitPage("failsafe");
            AbsSpecial::exitPage("failsafe");
            SpdoSpecial::exitPage("failsafe");
            RadSpecial::exitPage("failsafe");
            LhcSpecial::exitPage("failsafe");
        }
        MainWebServer::setLiveStream0200(false);
        UdsHandler::setTesterPresent(false, 0);
        UdsHandler::resetEcuFamilyLatch();
        settings.active_module_id = 0;
        settings.active_module_name[0] = '\0';
        server.send(200, "application/json", "{\"ok\":true,\"tester_present\":false}");
    };

    server.on("/api/module/exit", HTTP_POST, handleModuleExit);
    server.on("/api/uds/module/exit", HTTP_POST, handleModuleExit);

    // ------------------------------------------------------------------------
    // 5. POST /api/uds/read_did - Single Synchronous DID Query (with auto-retry)
    // ------------------------------------------------------------------------
    auto handleReadSingleDid = []() {
        uint32_t target_id = settings.active_module_id != 0 ? settings.active_module_id : 0x7E0;
        if (server.hasArg("id")) {
            String moduleIdArgText = server.arg("id");
            moduleIdArgText.trim();
            if (moduleIdArgText.startsWith("0x") || moduleIdArgText.startsWith("0X")) {
                target_id = (uint32_t)strtoul(moduleIdArgText.c_str(), nullptr, 16);
            } else {
                target_id = (uint32_t)moduleIdArgText.toInt();
            }
        }

        uint16_t did = HD_DID_VIN;
        if (server.hasArg("did")) {
            String didArg = server.arg("did");
            didArg.trim();
            if (didArg.startsWith("0x") || didArg.startsWith("0X")) {
                did = (uint16_t)strtoul(didArg.c_str(), nullptr, 16);
            } else {
                did = (uint16_t)didArg.toInt();
            }
        }

        Serial.printf("[WEB] POST /api/uds/read_did -> Target ECU: 0x%03X, DID: 0x%04X\n", target_id, did);

        // If requesting DID 0x0200 (Read Module Inputs) for ABS (0x7E1), BCM (0x7E2), or ECM (0x7E0), parse & broadcast via WebSocket
        if (did == HD_DID_ECM_LIVE_INPUTS) {
            if (target_id == HD_ECU_ABS) {
                bool gotAbs = UdsHandler::readAbsInputs0200Synchronous(target_id, s_web_abs_inputs, 650);
                if (gotAbs) {
                    MainWebServer::broadcastAbsInputs0200(s_web_abs_inputs);
                    MainWebServer::serializeAbsInputs0200Json(s_web_abs_inputs, s_http_abs_json, sizeof(s_http_abs_json), true);
                    server.send(200, "application/json", s_http_abs_json);
                    return;
                }
                server.send(200, "application/json", "{\"ok\":false,\"no_reply\":true,\"module\":\"abs\",\"did\":\"0200\",\"error\":\"No ECU Response\"}");
                return;
            }
            if (target_id == HD_ECU_BCM) {
                bool gotBcm = UdsHandler::readBcmInputs0200Synchronous(target_id, s_web_bcm_inputs, 650);
                if (gotBcm) {
                    MainWebServer::broadcastBcmInputs0200(s_web_bcm_inputs);
                    MainWebServer::serializeBcmInputs0200Json(s_web_bcm_inputs, s_http_bcm_json, sizeof(s_http_bcm_json), true);
                    server.send(200, "application/json", s_http_bcm_json);
                    return;
                }
                server.send(200, "application/json", "{\"ok\":false,\"no_reply\":true,\"module\":\"bcm\",\"did\":\"0200\",\"error\":\"No ECU Response\"}");
                return;
            }

            if (findGenModule(target_id)) {
                if (UdsHandler::readGenInputs0200Synchronous(target_id, s_web_gen_inputs, 650)) {
                    MainWebServer::broadcastGenInputs0200(s_web_gen_inputs);
                    MainWebServer::serializeGenInputs0200Json(s_web_gen_inputs, s_http_gen_json, sizeof(s_http_gen_json), true);
                    server.send(200, "application/json", s_http_gen_json);
                } else {
                    server.send(200, "application/json", "{\"ok\":false,\"no_reply\":true,\"did\":\"0200\",\"error\":\"No ECU Response\"}");
                }
                return;
            }

            bool got0200 = UdsHandler::readEcmInputs0200Synchronous(target_id, s_web_ecm_inputs, 650);
            if (got0200) {
                MainWebServer::broadcastEcmInputs0200(s_web_ecm_inputs);
                MainWebServer::serializeEcmInputs0200Json(s_web_ecm_inputs, s_http_ecm_json, sizeof(s_http_ecm_json), true);
                server.send(200, "application/json", s_http_ecm_json);
                return;
            }
            server.send(200, "application/json", "{\"ok\":false,\"no_reply\":true,\"module\":\"ecm\",\"did\":\"0200\",\"error\":\"No ECU Response\"}");
            return;
        }

        char readValue[48] = {0};
        UdsHandler::setDiagnosticSequenceActive(true);
        bool got = UdsHandler::readDidSynchronous(target_id, did, readValue, sizeof(readValue), 600);
        UdsHandler::setDiagnosticSequenceActive(false);

        char json[256];
        snprintf(
            json,
            sizeof(json),
            "{\"ok\":true,\"module_id\":\"0x%03X\",\"module_name\":\"%s\",\"did\":\"0x%04X\",\"got\":%s,\"value\":\"%s\"}",
            target_id,
            lookupModuleName(target_id),
            did,
            got ? "true" : "false",
            got ? readValue : "N/A (No ECU Response)"
        );
        server.send(200, "application/json", json);
    };
    server.on("/api/uds/read_did", HTTP_POST, handleReadSingleDid);
    // ------------------------------------------------------------------------
    // Read ECU Information (database driven, see ecu_info.h)
    //     POST /api/ecu/info/plan?id=0x7E2          -> item list chosen from the module identification string
    //     POST /api/ecu/info/item?id=0x7E2&i=<n>    -> read + decode one item
    // ------------------------------------------------------------------------
    auto ecuInfoModuleId = []() -> uint32_t {
        uint32_t moduleId = settings.active_module_id != 0 ? settings.active_module_id : 0x7E0;
        if (server.hasArg("id")) {
            String a = server.arg("id");
            a.trim();
            moduleId = (a.startsWith("0x") || a.startsWith("0X")) ? (uint32_t)strtoul(a.c_str(), nullptr, 16) : (uint32_t)a.toInt();
        }
        return moduleId;
    };
    server.on("/api/ecu/info/plan", HTTP_POST, [ecuInfoModuleId]() {
        char *responseBuffer = g_apiBufMed;
        MainWebServer::setLiveStream0200(false);
        EcuInfo::plan(ecuInfoModuleId(), responseBuffer, sizeof(g_apiBufMed));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/ecu/vin", HTTP_POST, [ecuInfoModuleId]() {
        char *responseBuffer = g_apiBufSmall;
        EcuInfo::vinJson(ecuInfoModuleId(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/ecu/info/item", HTTP_POST, [ecuInfoModuleId]() {
        char *responseBuffer = g_apiBufSmall;
        int itemIndex = server.hasArg("i") ? server.arg("i").toInt() : 0;
        EcuInfo::item(ecuInfoModuleId(), (uint8_t)(itemIndex < 0 ? 0 : itemIndex), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });

    server.on("/api/uds/read_did", HTTP_GET, handleReadSingleDid);

    // ------------------------------------------------------------------------
    // 5.2 POST /api/uds/ecu_info - Sequential DID Query: 0xF1EF, 0xF1EA, 0xF1ED, 0xF1EE
    // ------------------------------------------------------------------------
    server.on("/api/uds/ecu_info", HTTP_POST, []() {
        uint32_t target_id = settings.active_module_id != 0 ? settings.active_module_id : 0x7E0;
        if (server.hasArg("id")) {
            String moduleIdArgText = server.arg("id");
            moduleIdArgText.trim();
            if (moduleIdArgText.startsWith("0x") || moduleIdArgText.startsWith("0X")) {
                target_id = (uint32_t)strtoul(moduleIdArgText.c_str(), nullptr, 16);
            } else {
                target_id = (uint32_t)moduleIdArgText.toInt();
            }
        }

        Serial.printf("[WEB] POST /api/uds/ecu_info -> Reading all 4 DIDs from ECU 0x%03X\n", target_id);

        char vin[48] = {0};
        char parts[48] = {0};
        char calib[48] = {0};
        char homol[48] = {0};

        // Hold diagnostic sequence lock across all 4 DID reads so background TesterPresent (0x3E) never collides
        UdsHandler::setDiagnosticSequenceActive(true);

        bool gotVin   = UdsHandler::readDidSynchronous(target_id, HD_DID_VIN, vin, sizeof(vin), 600);
        delay(60);
        bool gotParts = UdsHandler::readDidSynchronous(target_id, HD_DID_PARTS_NUMBER, parts, sizeof(parts), 600);
        delay(60);
        bool gotCalib = UdsHandler::readDidSynchronous(target_id, HD_DID_CALIBRATION_F193, calib, sizeof(calib), 600);
        if (!gotCalib) {
            delay(40);
            gotCalib = UdsHandler::readDidSynchronous(target_id, HD_DID_CALIBRATION_ID, calib, sizeof(calib), 600);
        }
        if (gotCalib) {
            if (target_id == HD_ECU_ECM) UdsHandler::identifyEcuFamily(calib);
            else if (target_id == HD_ECU_ABS) UdsHandler::identifyAbsFamily(calib);
            else if (target_id == HD_ECU_BCM) UdsHandler::identifyBcmFamily(calib);
            else if (findGenModule(target_id)) UdsHandler::identifyGenLayout(target_id, calib);
        }
        delay(60);
        bool gotHomol = UdsHandler::readDidSynchronous(target_id, HD_DID_HOMOLOGATION_ID, homol, sizeof(homol), 600);

        UdsHandler::setDiagnosticSequenceActive(false);

        char json[512];
        snprintf(
            json,
            sizeof(json),
            "{\"ok\":true,\"module_id\":\"0x%03X\",\"module_name\":\"%s\","
            "\"vin\":\"%s\",\"parts_number\":\"%s\",\"calibration_id\":\"%s\",\"homologation_id\":\"%s\","
            "\"status\":{\"vin\":%s,\"parts\":%s,\"calibration\":%s,\"homologation\":%s}}",
            target_id,
            lookupModuleName(target_id),
            gotVin ? vin : "N/A (No ECU Response)",
            gotParts ? parts : "N/A (No ECU Response)",
            gotCalib ? calib : "N/A (No ECU Response)",
            gotHomol ? homol : "N/A (No ECU Response)",
            gotVin ? "true" : "false",
            gotParts ? "true" : "false",
            gotCalib ? "true" : "false",
            gotHomol ? "true" : "false"
        );
        server.send(200, "application/json", json);
    });

    // ------------------------------------------------------------------------
    // 5.5 POST /api/uds/search_modules - Broadcast TesterPresent (0x7DF) Scan
    // ------------------------------------------------------------------------
    server.on("/api/uds/search_modules", HTTP_POST, []() {
        Serial.println("[WEB] POST /api/uds/search_modules -> Initiating 4000 ms CAN broadcast scan (0x7DF)");
        uint32_t detectedModuleIds[16] = {0};
        uint8_t count = 0;
        // Wait up to 4 seconds (4000 ms) for all modules to reply - some ECUs answer late.
        UdsHandler::searchModulesSynchronous(detectedModuleIds, 16, &count, 2000);

        // Sort detected modules strictly in ascending order of their ID address
        for (uint8_t i = 0; i < count; i++) {
            for (uint8_t j = i + 1; j < count; j++) {
                if (detectedModuleIds[j] < detectedModuleIds[i]) {
                    uint32_t swapTemp = detectedModuleIds[i];
                    detectedModuleIds[i] = detectedModuleIds[j];
                    detectedModuleIds[j] = swapTemp;
                }
            }
        }

        String json;
        json.reserve(count * 150 + 64);
        json = "{\"ok\":true,\"count\":" + String(count) + ",\"modules\":[";
        for (uint8_t i = 0; i < count; i++) {
            uint32_t moduleId = detectedModuleIds[i];
            char moduleIdHex[10];
            char responseIdHex[10];
            snprintf(moduleIdHex, sizeof(moduleIdHex), "0x%03X", moduleId);
            snprintf(responseIdHex, sizeof(responseIdHex), "0x%03X", moduleId + 8);

            const char *acronym = "";
            String moduleDescriptionText = "";
            bool isPrimary = false;
            if (moduleId == HD_ECU_ECM) { acronym = "ECM"; moduleDescriptionText = "ECM (Engine Control Module)"; isPrimary = true; }
            else if (moduleId == HD_ECU_ABS) { acronym = "ABS"; moduleDescriptionText = "ABS (Anti-lock Braking System)"; isPrimary = true; }
            else if (moduleId == HD_ECU_BCM) { acronym = "BCM"; moduleDescriptionText = "BCM (Body Control Module)"; isPrimary = true; }
            else if (moduleId == HD_ECU_LHC) { acronym = "LHC"; moduleDescriptionText = "LHC (Left Hand Controls)"; isPrimary = true; }
            else if (moduleId == HD_ECU_RHC) { acronym = "RHC"; moduleDescriptionText = "RHC (Right Hand Controls)"; isPrimary = true; }
            else if (moduleId == HD_ECU_RADIO) { acronym = "RAD"; moduleDescriptionText = "RAD (Radio / Infotainment Module)"; isPrimary = true; }
            else if (moduleId == HD_ECU_SPEEDO) { acronym = "SPDO"; moduleDescriptionText = "SPDO (Speedometer Module)"; isPrimary = true; }
            else if (moduleId == HD_ECU_SCM) { acronym = "SCM"; moduleDescriptionText = "SCM (Security Control Module)"; isPrimary = true; }
            else if (moduleId == HD_ECU_EHCU) { acronym = "EHCU"; moduleDescriptionText = "EHCU (Electro-Hydraulic Control Unit)"; isPrimary = true; }
            else {
                moduleDescriptionText = moduleIdHex;
            }

            if (i > 0) json += ",";
            json += "{\"id\":" + String(moduleId) + ",\"id_hex\":\"" + String(moduleIdHex) + "\",";
            json += "\"acronym\":\"" + String(acronym) + "\",";
            json += "\"description\":\"" + moduleDescriptionText + "\",";
            json += "\"name\":\"" + moduleDescriptionText + "\",";
            json += "\"is_primary\":" + String(isPrimary ? "true" : "false") + ",";
            json += "\"response_id\":\"" + String(responseIdHex) + "\"}";
        }
        json += "]}";
        server.send(200, "application/json", json);
    });

    // ------------------------------------------------------------------------
    // 5.6 POST /api/uds/scan_all_dtc - Single-Button Broadcast Discovery + DTC Scan
    //     Detects all responding HD ECUs (0x7DF) and reads DTC count (0x19) for each
    // ------------------------------------------------------------------------
    server.on("/api/uds/scan_all_dtc", HTTP_POST, []() {
        Serial.println("[WEB] POST /api/uds/scan_all_dtc -> Detecting responding modules (0x7DF) and scanning DTCs (0x19)...");
        UdsHandler::setDiagnosticSequenceActive(true);

        uint32_t detectedModuleIds[16] = {0};
        uint8_t count = 0;
        UdsHandler::searchModulesSynchronous(detectedModuleIds, 16, &count, 2000);   // 2 s

        for (uint8_t i = 0; i < count; i++) {
            for (uint8_t j = i + 1; j < count; j++) {
                if (detectedModuleIds[j] < detectedModuleIds[i]) {
                    uint32_t swapTemp = detectedModuleIds[i];
                    detectedModuleIds[i] = detectedModuleIds[j];
                    detectedModuleIds[j] = swapTemp;
                }
            }
        }

        uint16_t totalDtcCount = 0;
        String json;
        json.reserve(count * 150 + 64);
        json = "{\"ok\":true,\"count\":" + String(count) + ",\"modules\":[";
        for (uint8_t i = 0; i < count; i++) {
            uint32_t moduleId = detectedModuleIds[i];
            delay(50);
            uint8_t dtcCount = 0;
            bool responded = UdsHandler::readDtcSynchronous(moduleId, s_web_dtcs, 16, &dtcCount, 1200);
            if (!responded) dtcCount = 0;
            totalDtcCount += dtcCount;

            char moduleIdHex[10];
            char responseIdHex[10];
            snprintf(moduleIdHex, sizeof(moduleIdHex), "0x%03X", moduleId);
            snprintf(responseIdHex, sizeof(responseIdHex), "0x%03X", moduleId + 8);

            const char *acronym = "";
            String moduleDescriptionText = "";
            bool isPrimary = false;
            if (moduleId == HD_ECU_ECM) { acronym = "ECM"; moduleDescriptionText = "ECM (Engine Control Module)"; isPrimary = true; }
            else if (moduleId == HD_ECU_ABS) { acronym = "ABS"; moduleDescriptionText = "ABS (Anti-lock Braking System)"; isPrimary = true; }
            else if (moduleId == HD_ECU_BCM) { acronym = "BCM"; moduleDescriptionText = "BCM (Body Control Module)"; isPrimary = true; }
            else if (moduleId == HD_ECU_LHC) { acronym = "LHC"; moduleDescriptionText = "LHC (Left Hand Controls)"; isPrimary = true; }
            else if (moduleId == HD_ECU_RHC) { acronym = "RHC"; moduleDescriptionText = "RHC (Right Hand Controls)"; isPrimary = true; }
            else if (moduleId == HD_ECU_RADIO) { acronym = "RAD"; moduleDescriptionText = "RAD (Radio / Infotainment Module)"; isPrimary = true; }
            else if (moduleId == HD_ECU_SPEEDO) { acronym = "SPDO"; moduleDescriptionText = "SPDO (Speedometer Module)"; isPrimary = true; }
            else if (moduleId == HD_ECU_SCM) { acronym = "SCM"; moduleDescriptionText = "SCM (Security Control Module)"; isPrimary = true; }
            else if (moduleId == HD_ECU_EHCU) { acronym = "EHCU"; moduleDescriptionText = "EHCU (Electro-Hydraulic Control Unit)"; isPrimary = true; }
            else {
                moduleDescriptionText = moduleIdHex;
            }

            if (i > 0) json += ",";
            json += "{\"id\":" + String(moduleId) + ",\"id_hex\":\"" + String(moduleIdHex) + "\",";
            json += "\"acronym\":\"" + String(acronym) + "\",";
            json += "\"description\":\"" + moduleDescriptionText + "\",";
            json += "\"name\":\"" + moduleDescriptionText + "\",";
            json += "\"is_primary\":" + String(isPrimary ? "true" : "false") + ",";
            json += "\"response_id\":\"" + String(responseIdHex) + "\",";
            json += "\"dtc_count\":" + String(dtcCount) + "}";
        }
        json += "],\"total_dtcs\":" + String(totalDtcCount) + "}";

        UdsHandler::setDiagnosticSequenceActive(false);
        server.send(200, "application/json", json);
    });

    // ------------------------------------------------------------------------
    // 6. POST /api/uds/read_dtc - Read Diagnostic Trouble Codes (Service 0x19)
    // ------------------------------------------------------------------------
    server.on("/api/uds/read_dtc", HTTP_POST, []() {
        uint32_t target_id = settings.active_module_id != 0 ? settings.active_module_id : 0x7E0;
        if (server.hasArg("id")) {
            String moduleIdArgText = server.arg("id");
            if (moduleIdArgText.startsWith("0x") || moduleIdArgText.startsWith("0X")) {
                target_id = (uint32_t)strtoul(moduleIdArgText.c_str(), nullptr, 16);
            } else {
                target_id = (uint32_t)moduleIdArgText.toInt();
            }
        }

        Serial.printf("[WEB] POST /api/uds/read_dtc -> Reading DTCs from ECU 0x%03X\n", target_id);

        uint8_t count = 0;
        bool responded = UdsHandler::readDtcSynchronous(target_id, s_web_dtcs, 16, &count, 1200);

        char modHex[10];
        snprintf(modHex, sizeof(modHex), "0x%03X", target_id);

        if (!responded) {
            String json = "{\"ok\":false,\"no_reply\":true,\"module_id\":\"" + String(modHex) + "\",";
            json += "\"module_name\":\"" + String(lookupModuleName(target_id)) + "\",";
            json += "\"error\":\"No ECU Response\",\"message\":\"No reply received from module on CAN bus.\"}";
            server.send(200, "application/json", json);
            return;
        }

        String json;
        json.reserve(count * 180 + 160);
        json = "{\"ok\":true,\"module_id\":\"" + String(modHex) + "\",";
        json += "\"module_name\":\"" + String(lookupModuleName(target_id)) + "\",";
        json += "\"count\":" + String(count) + ",\"dtcs\":[";

        for (uint8_t i = 0; i < count; i++) {
            if (i > 0) json += ",";
            json += "{\"code\":\"" + String(s_web_dtcs[i].code) + "\",";
            json += "\"raw_hex\":\"" + String(s_web_dtcs[i].raw_hex) + "\",";
            json += "\"status_byte\":" + String(s_web_dtcs[i].status_byte) + ",";
            json += "\"status_desc\":\"" + String(s_web_dtcs[i].status_desc) + "\",";
            json += "\"description\":\"" + String(s_web_dtcs[i].description) + "\"}";
        }
        json += "],\"message\":\"" + String(count > 0 ? "Fault codes detected in ECU memory." : "No fault codes stored in module memory.") + "\"}";
        server.send(200, "application/json", json);
    });

    // ------------------------------------------------------------------------
    // 7. POST /api/uds/clear_dtc - Clear Diagnostic Trouble Codes (Service 0x14)
    // ------------------------------------------------------------------------
    server.on("/api/uds/clear_dtc", HTTP_POST, []() {
        uint32_t target_id = settings.active_module_id != 0 ? settings.active_module_id : 0x7E0;
        if (server.hasArg("id")) {
            String moduleIdArgText = server.arg("id");
            if (moduleIdArgText.startsWith("0x") || moduleIdArgText.startsWith("0X")) {
                target_id = (uint32_t)strtoul(moduleIdArgText.c_str(), nullptr, 16);
            } else {
                target_id = (uint32_t)moduleIdArgText.toInt();
            }
        }

        Serial.printf("[WEB] POST /api/uds/clear_dtc -> Clearing DTCs on ECU 0x%03X\n", target_id);

        bool cleared = UdsHandler::clearDtcSynchronous(target_id, 1000);

        char modHex[10];
        snprintf(modHex, sizeof(modHex), "0x%03X", target_id);

        if (!cleared) {
            String json = "{\"ok\":false,\"no_reply\":true,\"module_id\":\"" + String(modHex) + "\",";
            json += "\"module_name\":\"" + String(lookupModuleName(target_id)) + "\",";
            json += "\"error\":\"No ECU Response\",\"message\":\"No reply received or negative response from module on CAN bus.\"}";
            server.send(200, "application/json", json);
            return;
        }

        char json[256];
        snprintf(
            json,
            sizeof(json),
            "{\"ok\":true,\"module_id\":\"0x%03X\",\"module_name\":\"%s\",\"cleared\":true,\"message\":\"Clear DTC command (Service 0x14) acknowledged by ECU.\"}",
            target_id,
            lookupModuleName(target_id)
        );
        server.send(200, "application/json", json);
    });

    // ------------------------------------------------------------------------
    // 8. Read Module Inputs (DID 0x0200 - ECM 0x7E0 [41 Params], ABS 0x7E1 [13 Params], BCM 0x7E2 [12 Params])
    // ------------------------------------------------------------------------
    auto handleReadModuleInputs = []() {
        uint32_t target_id = settings.active_module_id != 0 ? settings.active_module_id : HD_ECU_ECM;
        bool hasExplicitId = false;
        if (server.hasArg("id")) {
            String moduleIdArgText = server.arg("id");
            moduleIdArgText.trim();
            if (moduleIdArgText.length() > 0) {
                if (moduleIdArgText.startsWith("0x") || moduleIdArgText.startsWith("0X")) {
                    target_id = (uint32_t)strtoul(moduleIdArgText.c_str(), nullptr, 16);
                } else {
                    target_id = (uint32_t)moduleIdArgText.toInt();
                }
                if (target_id >= HD_MIN_DIAG_ID && target_id <= HD_MAX_DIAG_ID) {
                    hasExplicitId = true;
                }
            }
        }
        if (!hasExplicitId && server.hasArg("module")) {
            String modArg = server.arg("module");
            modArg.trim();
            if (modArg.equalsIgnoreCase("abs")) {
                target_id = HD_ECU_ABS;
            } else if (modArg.equalsIgnoreCase("bcm")) {
                target_id = HD_ECU_BCM;
            } else if (modArg.equalsIgnoreCase("lhc")) {
                target_id = HD_ECU_LHC;
            } else if (modArg.equalsIgnoreCase("rhc")) {
                target_id = HD_ECU_RHC;
            } else if (modArg.equalsIgnoreCase("rad") || modArg.equalsIgnoreCase("radio")) {
                target_id = HD_ECU_RADIO;
            } else if (modArg.equalsIgnoreCase("spdo") || modArg.equalsIgnoreCase("speedo")) {
                target_id = HD_ECU_SPEEDO;
            } else if (modArg.equalsIgnoreCase("scm")) {
                target_id = HD_ECU_SCM;
            } else if (modArg.equalsIgnoreCase("ehcu")) {
                target_id = HD_ECU_EHCU;
            } else if (modArg.equalsIgnoreCase("ecm")) {
                target_id = HD_ECU_ECM;
            }
        }

        if (server.hasArg("stream")) {
            String st = server.arg("stream");
            live_stream_0200_module = target_id;
            MainWebServer::setLiveStream0200(st == "1" || st == "true" || st == "start");
        }

        if (target_id == HD_ECU_ABS) {
            Serial.printf("[WEB] /api/uds/inputs -> Querying ABS DID 0x0200 (ABS Live Sensor Inputs) from ECU 0x%03X\n", target_id);
            bool got = UdsHandler::readAbsInputs0200Synchronous(target_id, s_web_abs_inputs, 650);
            last_live_stream_0200_ms = millis();
            if (got) {
                MainWebServer::broadcastAbsInputs0200(s_web_abs_inputs);
                MainWebServer::serializeAbsInputs0200Json(s_web_abs_inputs, s_http_abs_json, sizeof(s_http_abs_json), true);
                server.send(200, "application/json", s_http_abs_json);
                return;
            }

            char errAbs[256];
            snprintf(
                errAbs,
                sizeof(errAbs),
                "{\"ok\":false,\"no_reply\":true,\"type\":\"live_data\",\"module\":\"abs\",\"module_id\":\"0x%03X\",\"did\":\"0200\",\"error\":\"No ECU Response\"}",
                target_id
            );
            server.send(200, "application/json", errAbs);
            return;
        }

        if (target_id == HD_ECU_BCM) {
            Serial.printf("[WEB] /api/uds/inputs -> Querying BCM DID 0x0200 (BCM Live Body & Security Inputs) from ECU 0x%03X\n", target_id);
            bool got = UdsHandler::readBcmInputs0200Synchronous(target_id, s_web_bcm_inputs, 650);
            last_live_stream_0200_ms = millis();
            if (got) {
                MainWebServer::broadcastBcmInputs0200(s_web_bcm_inputs);
                MainWebServer::serializeBcmInputs0200Json(s_web_bcm_inputs, s_http_bcm_json, sizeof(s_http_bcm_json), true);
                server.send(200, "application/json", s_http_bcm_json);
                return;
            }

            char errBcm[256];
            snprintf(
                errBcm,
                sizeof(errBcm),
                "{\"ok\":false,\"no_reply\":true,\"type\":\"live_data\",\"module\":\"bcm\",\"module_id\":\"0x%03X\",\"did\":\"0200\",\"error\":\"No ECU Response\"}",
                target_id
            );
            server.send(200, "application/json", errBcm);
            return;
        }

        if (findGenModule(target_id)) {
            bool gotGen = UdsHandler::readGenInputs0200Synchronous(target_id, s_web_gen_inputs, 650);
            last_live_stream_0200_ms = millis();
            if (gotGen) {
                MainWebServer::broadcastGenInputs0200(s_web_gen_inputs);
                MainWebServer::serializeGenInputs0200Json(s_web_gen_inputs, s_http_gen_json, sizeof(s_http_gen_json), true);
                server.send(200, "application/json", s_http_gen_json);
            } else {
                char errGen[256];
                snprintf(errGen, sizeof(errGen),
                    "{\"ok\":false,\"no_reply\":true,\"type\":\"live_data\",\"module\":\"%s\",\"module_id\":\"0x%03X\",\"did\":\"0200\",\"error\":\"No ECU Response\"}",
                    lookupModuleName(target_id), target_id);
                server.send(200, "application/json", errGen);
            }
            return;
        }

        if (target_id != HD_ECU_ECM) {
            Serial.printf("[WEB] /api/uds/inputs -> Module 0x%03X (%s) does not use ECM 0x0200 profile.\n", target_id, lookupModuleName(target_id));
            char errOther[256];
            snprintf(
                errOther,
                sizeof(errOther),
                "{\"ok\":false,\"no_reply\":true,\"type\":\"live_data\",\"module\":\"%s\",\"module_id\":\"0x%03X\",\"did\":\"0200\",\"error\":\"DID 0x0200 not supported on this module\"}",
                lookupModuleName(target_id),
                target_id
            );
            server.send(200, "application/json", errOther);
            return;
        }

        Serial.printf("[WEB] /api/uds/inputs -> Querying ECM DID 0x0200 (41 Parameters) from ECU 0x%03X\n", target_id);

        bool got = UdsHandler::readEcmInputs0200Synchronous(target_id, s_web_ecm_inputs, 650);
        last_live_stream_0200_ms = millis();
        if (got) {
            MainWebServer::broadcastEcmInputs0200(s_web_ecm_inputs);
            MainWebServer::serializeEcmInputs0200Json(s_web_ecm_inputs, s_http_ecm_json, sizeof(s_http_ecm_json), true);
            server.send(200, "application/json", s_http_ecm_json);
            return;
        }

        char errJson[256];
        snprintf(
            errJson,
            sizeof(errJson),
            "{\"ok\":false,\"no_reply\":true,\"type\":\"live_data\",\"module\":\"ecm\",\"module_id\":\"0x%03X\",\"did\":\"0200\",\"error\":\"No ECU Response\"}",
            target_id
        );
        server.send(200, "application/json", errJson);
    };
    server.on("/api/uds/inputs", HTTP_POST, handleReadModuleInputs);
    server.on("/api/uds/inputs", HTTP_GET, handleReadModuleInputs);

    server.on("/api/uds/inputs/stream", HTTP_POST, []() {
        bool enable = true;
        if (server.hasArg("enable")) {
            String e = server.arg("enable");
            enable = (e == "1" || e == "true" || e == "start");
        }
        MainWebServer::setLiveStream0200(enable);
        server.send(200, "application/json", enable ? "{\"ok\":true,\"streaming\":true}" : "{\"ok\":true,\"streaming\":false}");
    });

    server.on("/api/uds/outputs", HTTP_POST, []() {
        uint32_t target_id = settings.active_module_id != 0 ? settings.active_module_id : HD_ECU_ECM;
        if (server.hasArg("id")) {
            String moduleIdArgText = server.arg("id");
            moduleIdArgText.trim();
            if (moduleIdArgText.startsWith("0x") || moduleIdArgText.startsWith("0X")) {
                target_id = (uint32_t)strtoul(moduleIdArgText.c_str(), nullptr, 16);
            } else {
                target_id = (uint32_t)moduleIdArgText.toInt();
            }
        }
        if (server.hasArg("module") && server.arg("module").equalsIgnoreCase("abs")) {
            target_id = HD_ECU_ABS;
        }

        uint16_t did = (target_id == HD_ECU_ABS) ? HD_DID_ACT_ABS_PUMP_MOTOR : HD_DID_ACT_FUEL_PUMP;
        if (server.hasArg("did")) {
            String didArg = server.arg("did");
            didArg.trim();
            if (didArg.startsWith("0x") || didArg.startsWith("0X")) {
                did = (uint16_t)strtoul(didArg.c_str(), nullptr, 16);
            } else {
                did = (uint16_t)strtoul(didArg.c_str(), nullptr, 16);
            }
        }
        uint8_t state = server.hasArg("state") ? (uint8_t)server.arg("state").toInt() : 1;

        if (!(target_id == HD_ECU_ABS || did == HD_DID_ACT_ABS_PUMP_MOTOR)) {
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"ECM output tests moved to /api/ecm/outputs\"}");
            return;
        }
        outputs_view_active = true;
        ActuatorOutputResult actuatorResult;
        bool succeeded = false;
        if (target_id == HD_ECU_ABS || did == HD_DID_ACT_ABS_PUMP_MOTOR) {
            abs_outputs_view_active = true;
            succeeded = UdsHandler::controlAbsActuatorOutput(did, state, actuatorResult, 600);
        } else {
            succeeded = UdsHandler::controlActuatorOutput(target_id, did, state, actuatorResult, 550);
        }

        char respJson[520];
        snprintf(
            respJson,
            sizeof(respJson),
            "{\"type\":\"actuator_response\",\"module\":\"%s\",\"target_id\":\"0x%03X\",\"ok\":%s,"
            "\"did\":\"%04X\",\"requested_state\":%u,\"state\":%u,\"pulse_ms\":%u,"
            "\"got_reply\":%s,\"is_positive\":%s,\"is_negative\":%s,\"rpm_interlock\":%s,\"speed_interlock\":%s,"
            "\"nrc\":%u,\"tx_hex\":\"%s\",\"rx_hex\":\"%s\",\"message\":\"%s\"}",
            (target_id == HD_ECU_ABS || did == HD_DID_ACT_ABS_PUMP_MOTOR) ? "abs" : "ecm",
            (target_id == HD_ECU_ABS || did == HD_DID_ACT_ABS_PUMP_MOTOR) ? HD_ECU_ABS : target_id,
            succeeded ? "true" : "false",
            actuatorResult.did,
            actuatorResult.requested_state,
            actuatorResult.current_state,
            actuatorResult.pulse_duration_ms,
            actuatorResult.got_reply ? "true" : "false",
            actuatorResult.is_positive ? "true" : "false",
            actuatorResult.is_negative ? "true" : "false",
            actuatorResult.rpm_interlock ? "true" : "false",
            actuatorResult.speed_interlock ? "true" : "false",
            actuatorResult.nrc,
            actuatorResult.tx_frame_hex,
            actuatorResult.rx_frame_hex,
            actuatorResult.message
        );
        MainWebServer::broadcastWsJson(respJson);
        server.send(200, "application/json", respJson);
    });

    server.on("/api/uds/outputs/cleanup", HTTP_POST, []() {
        uint32_t target_id = settings.active_module_id != 0 ? settings.active_module_id : HD_ECU_ECM;
        if (server.hasArg("module") && server.arg("module").equalsIgnoreCase("abs")) {
            target_id = HD_ECU_ABS;
        }
        outputs_view_active = false;
        if (abs_outputs_view_active || target_id == HD_ECU_ABS || UdsHandler::isAbsOutputsPageActive()) {
            abs_outputs_view_active = false;
            UdsHandler::cleanupAbsActuatorOutputs();
            const char *absCleanJson = "{\"type\":\"actuator_cleanup\",\"module\":\"abs\",\"ok\":true,"
                                       "\"reset_frame\":\"04 2F 05 40 00 00 00 00\",\"session_reset\":\"02 10 01 00 00 00 00 00\","
                                       "\"message\":\"ABS Hydraulic Pump Motor (0x0540) returned to ECU (04 2F 05 40 00 00 00 00), Default Session (02 10 01) restored, and 02 3E 80 TesterPresent stopped.\"}";
            MainWebServer::broadcastWsJson(absCleanJson);
            server.send(200, "application/json", absCleanJson);
            return;
        }
        EcmOutputs::exitPage("failsafe");
        BcmOutputs::exitPage("failsafe");
        AbsOutputs::exitPage("failsafe");
        SpdoOutputs::exitPage("failsafe");
        EcmSpecial::exitPage("failsafe");
        BcmSpecial::exitPage("failsafe");
        AbsSpecial::exitPage("failsafe");
        SpdoSpecial::exitPage("failsafe");
        RadSpecial::exitPage("failsafe");
        LhcSpecial::exitPage("failsafe");
        const char *ecmCleanJson ="{\"type\":\"actuator_cleanup\",\"module\":\"ecm\",\"ok\":true,\"session_reset\":\"02 10 01 00 00 00 00 00\","
                                   "\"message\":\"All 7 actuator DIDs returned to ECU (0x00) and Default Session (02 10 01) restored.\"}";
        MainWebServer::broadcastWsJson(ecmCleanJson);
        server.send(200, "application/json", ecmCleanJson);
    });

    // ------------------------------------------------------------------------
    // 8B. ECM output tests (database-driven engine, see ecm_outputs.cpp)
    //     GET  /api/ecm/outputs               -> open the page (identifies the profile) + catalogue
    //     POST /api/ecm/outputs/cmd           -> key=<id>&op=on|off|fire|stop|arm|set&value=<step>&neutral=1
    //     POST /api/ecm/outputs/exit          -> return control of everything + default session
    //     POST /api/ecm/outputs/profile       -> set=<n> (manual) or set=auto
    // ------------------------------------------------------------------------
    server.on("/api/ecm/outputs", HTTP_GET, []() {
        char *responseBuffer = g_apiBufMed;
        MainWebServer::setLiveStream0200(false);
        char statusMessage[96];
        EcmOutputs::enter(statusMessage, sizeof(statusMessage));
        size_t len = EcmOutputs::catalogJson(responseBuffer, sizeof(g_apiBufMed));
        if (len == 0) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"catalog too large\"}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/ecm/outputs/cmd", HTTP_POST, []() {
        String key = server.arg("key"), op = server.arg("op");
        int value = server.hasArg("value") ? server.arg("value").toInt() : 0;
        bool neutral = server.hasArg("neutral") && server.arg("neutral") == "1";
        char statusMessage[128];
        bool succeeded = EcmOutputs::command(key.c_str(), op.c_str(), value, neutral, statusMessage, sizeof(statusMessage));
        char out[220];
        snprintf(out, sizeof(out), "{\"ok\":%s,\"msg\":\"%s\"}", succeeded ? "true" : "false", statusMessage);
        server.send(200, "application/json", out);
    });
    server.on("/api/ecm/outputs/status", HTTP_GET, []() {
        char *responseBuffer = g_apiBufSmall;
        if (EcmOutputs::statusJson(responseBuffer, sizeof(g_apiBufSmall)) == 0) { server.send(500, "application/json", "{\"ok\":false}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/ecm/outputs/exit", HTTP_POST, []() {
        if (server.hasArg("keep")) EcmOutputs::returnAll("manual return");
        else EcmOutputs::exitPage("page closed");
        server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/ecm/outputs/profile", HTTP_POST, []() {
        String set = server.arg("set");
        bool succeeded = (set == "auto") ? EcmOutputs::setProfile(-1) : EcmOutputs::setProfile(set.toInt());
        char *responseBuffer = g_apiBufMed;
        if (!succeeded || EcmOutputs::catalogJson(responseBuffer, sizeof(g_apiBufMed)) == 0) { server.send(400, "application/json", "{\"ok\":false}"); return; }
        server.send(200, "application/json", responseBuffer);
    });

    // ------------------------------------------------------------------------
    // BCM output tests (database driven, see bcm_outputs.h)
    //     GET  /api/bcm/outputs          -> open the page (identifies the profile from the Parts Number) + catalogue
    //     POST /api/bcm/outputs/cmd      -> key=<id>&op=on|off|fire|stop
    //     POST /api/bcm/outputs/exit     -> return control of everything + default session (keep=1: stay on the page)
    //     POST /api/bcm/outputs/profile  -> set=<n> (manual) or set=auto
    // ------------------------------------------------------------------------
    server.on("/api/bcm/outputs", HTTP_GET, []() {
        char *responseBuffer = g_apiBufMed;
        MainWebServer::setLiveStream0200(false);
        char statusMessage[96];
        BcmOutputs::enter(statusMessage, sizeof(statusMessage));
        if (BcmOutputs::catalogJson(responseBuffer, sizeof(g_apiBufMed)) == 0) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"catalog too large\"}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/bcm/outputs/cmd", HTTP_POST, []() {
        String key = server.arg("key"), op = server.arg("op");
        char statusMessage[128];
        bool succeeded = BcmOutputs::command(key.c_str(), op.c_str(), statusMessage, sizeof(statusMessage));
        char out[220];
        snprintf(out, sizeof(out), "{\"ok\":%s,\"msg\":\"%s\"}", succeeded ? "true" : "false", statusMessage);
        server.send(200, "application/json", out);
    });
    server.on("/api/bcm/outputs/status", HTTP_GET, []() {
        char *responseBuffer = g_apiBufSmall;
        if (BcmOutputs::statusJson(responseBuffer, sizeof(g_apiBufSmall)) == 0) { server.send(500, "application/json", "{\"ok\":false}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/bcm/outputs/exit", HTTP_POST, []() {
        if (server.hasArg("keep")) BcmOutputs::returnAll("manual return");
        else BcmOutputs::exitPage("page closed");
        server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/bcm/outputs/profile", HTTP_POST, []() {
        String set = server.arg("set");
        bool succeeded = (set == "auto") ? BcmOutputs::setProfile(-1) : BcmOutputs::setProfile(set.toInt());
        char *responseBuffer = g_apiBufMed;
        if (!succeeded || BcmOutputs::catalogJson(responseBuffer, sizeof(g_apiBufMed)) == 0) { server.send(400, "application/json", "{\"ok\":false}"); return; }
        server.send(200, "application/json", responseBuffer);
    });

    // ------------------------------------------------------------------------
    // ABS output tests (database driven, see abs_outputs.h)
    //     GET  /api/abs/outputs          -> open the page (identifies the profile from the Calibration ID) + catalogue
    //     POST /api/abs/outputs/cmd      -> key=<id>&op=on|off|fire|stop
    //     POST /api/abs/outputs/exit     -> return control of everything + default session (keep=1: stay on the page)
    //     POST /api/abs/outputs/profile  -> set=<n> (manual) or set=auto
    // ------------------------------------------------------------------------
    server.on("/api/abs/outputs", HTTP_GET, []() {
        char *responseBuffer = g_apiBufMed;
        MainWebServer::setLiveStream0200(false);
        char statusMessage[96];
        AbsOutputs::enter(statusMessage, sizeof(statusMessage));
        if (AbsOutputs::catalogJson(responseBuffer, sizeof(g_apiBufMed)) == 0) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"catalog too large\"}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/abs/outputs/cmd", HTTP_POST, []() {
        String key = server.arg("key"), op = server.arg("op");
        char statusMessage[128];
        bool succeeded = AbsOutputs::command(key.c_str(), op.c_str(), statusMessage, sizeof(statusMessage));
        char out[220];
        snprintf(out, sizeof(out), "{\"ok\":%s,\"msg\":\"%s\"}", succeeded ? "true" : "false", statusMessage);
        server.send(200, "application/json", out);
    });
    server.on("/api/abs/outputs/status", HTTP_GET, []() {
        char *responseBuffer = g_apiBufSmall;
        if (AbsOutputs::statusJson(responseBuffer, sizeof(g_apiBufSmall)) == 0) { server.send(500, "application/json", "{\"ok\":false}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/abs/outputs/exit", HTTP_POST, []() {
        if (server.hasArg("keep")) AbsOutputs::returnAll("manual return");
        else AbsOutputs::exitPage("page closed");
        server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/abs/outputs/profile", HTTP_POST, []() {
        String set = server.arg("set");
        bool succeeded = (set == "auto") ? AbsOutputs::setProfile(-1) : AbsOutputs::setProfile(set.toInt());
        char *responseBuffer = g_apiBufMed;
        if (!succeeded || AbsOutputs::catalogJson(responseBuffer, sizeof(g_apiBufMed)) == 0) { server.send(400, "application/json", "{\"ok\":false}"); return; }
        server.send(200, "application/json", responseBuffer);
    });

    // ------------------------------------------------------------------------
    // SPDO output tests (database driven, see spdo_outputs.h)
    //     GET  /api/spdo/outputs          -> open the page (identifies the profile from the Parts Number) + catalogue
    //     POST /api/spdo/outputs/cmd      -> key=<id>&op=on|off|stop|set&value=<step>
    //     POST /api/spdo/outputs/exit     -> return control of everything + default session (keep=1: stay on the page)
    //     POST /api/spdo/outputs/profile  -> set=<n> (manual) or set=auto
    // ------------------------------------------------------------------------
    server.on("/api/spdo/outputs", HTTP_GET, []() {
        char *responseBuffer = g_apiBufMed;
        MainWebServer::setLiveStream0200(false);
        char statusMessage[96];
        SpdoOutputs::enter(statusMessage, sizeof(statusMessage));
        if (SpdoOutputs::catalogJson(responseBuffer, sizeof(g_apiBufMed)) == 0) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"catalog too large\"}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/spdo/outputs/cmd", HTTP_POST, []() {
        String key = server.arg("key"), op = server.arg("op");
        int value = server.hasArg("value") ? server.arg("value").toInt() : 0;
        char statusMessage[128];
        bool succeeded = SpdoOutputs::command(key.c_str(), op.c_str(), value, statusMessage, sizeof(statusMessage));
        char out[220];
        snprintf(out, sizeof(out), "{\"ok\":%s,\"msg\":\"%s\"}", succeeded ? "true" : "false", statusMessage);
        server.send(200, "application/json", out);
    });
    server.on("/api/spdo/outputs/status", HTTP_GET, []() {
        char *responseBuffer = g_apiBufSmall;
        if (SpdoOutputs::statusJson(responseBuffer, sizeof(g_apiBufSmall)) == 0) { server.send(500, "application/json", "{\"ok\":false}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/spdo/outputs/exit", HTTP_POST, []() {
        if (server.hasArg("keep")) SpdoOutputs::returnAll("manual return");
        else SpdoOutputs::exitPage("page closed");
        server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/spdo/outputs/profile", HTTP_POST, []() {
        String set = server.arg("set");
        bool succeeded = (set == "auto") ? SpdoOutputs::setProfile(-1) : SpdoOutputs::setProfile(set.toInt());
        char *responseBuffer = g_apiBufMed;
        if (!succeeded || SpdoOutputs::catalogJson(responseBuffer, sizeof(g_apiBufMed)) == 0) { server.send(400, "application/json", "{\"ok\":false}"); return; }
        server.send(200, "application/json", responseBuffer);
    });

    // ------------------------------------------------------------------------
    // ECM special functions (database driven, see ecm_special.h)
    //     GET  /api/ecm/special               -> open the page (identifies the profile) + function list
    //     POST /api/ecm/special/read          -> key=<id>                      current value / state
    //     POST /api/ecm/special/run           -> key=<id>&value=<n>            execute (0/1, rpm, raw value, confirm=1 for pairing)
    //     POST /api/ecm/special/restore       -> key=<id>                      write back the original configuration block
    //     GET  /api/ecm/special/status        -> session / progress
    //     POST /api/ecm/special/exit          -> default session
    //     POST /api/ecm/special/profile       -> set=<n> | auto
    // ------------------------------------------------------------------------
    server.on("/api/ecm/special", HTTP_GET, []() {
        char *responseBuffer = g_apiBufBig;
        MainWebServer::setLiveStream0200(false);
        char statusMessage[96];
        EcmSpecial::enter(statusMessage, sizeof(statusMessage));
        if (EcmSpecial::catalogJson(responseBuffer, sizeof(g_apiBufBig)) == 0) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"catalog too large\"}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/ecm/special/read", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        EcmSpecial::read(server.arg("key").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/ecm/special/run", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        int value = server.hasArg("value") ? server.arg("value").toInt() : 0;
        if (server.hasArg("confirm") && server.arg("confirm") == "1" && !server.hasArg("value")) value = 1;
        EcmSpecial::run(server.arg("key").c_str(), value, responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/ecm/special/restore", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        EcmSpecial::restore(server.arg("key").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/ecm/special/status", HTTP_GET, []() {
        char *responseBuffer = g_apiBufSmall;
        if (EcmSpecial::statusJson(responseBuffer, sizeof(g_apiBufSmall)) == 0) { server.send(500, "application/json", "{\"ok\":false}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/ecm/special/exit", HTTP_POST, []() {
        EcmSpecial::exitPage("page closed");
        server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/ecm/special/profile", HTTP_POST, []() {
        String set = server.arg("set");
        bool succeeded = (set == "auto") ? EcmSpecial::setProfile(-1) : EcmSpecial::setProfile(set.toInt());
        char *responseBuffer = g_apiBufBig;
        if (!succeeded || EcmSpecial::catalogJson(responseBuffer, sizeof(g_apiBufBig)) == 0) { server.send(400, "application/json", "{\"ok\":false}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    // Standalone actions (always offered, not calibration-scoped) - the former ecm_tps_reset / ecm_relearn /
    // vin_write_ecm guided procedures, now plain one-shot actions like every other ECM special function.
    server.on("/api/ecm/special/tps_reset", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        EcmSpecial::runTpsReset(responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/ecm/special/relearn", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        EcmSpecial::runClearAdaptations(responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/ecm/special/vin/read", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        EcmSpecial::readVin(responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/ecm/special/vin/write", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        EcmSpecial::writeVin(server.arg("vin").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });

    // ------------------------------------------------------------------------
    // BCM special functions (configuration options, see bcm_special.h)
    //     GET  /api/bcm/special          -> open the page + function list
    //     POST /api/bcm/special/read     -> key=<id>
    //     POST /api/bcm/special/run      -> key=<id>&value=<option index>[&confirm=1]
    //     POST /api/bcm/special/restore  -> key=<id>
    //     POST /api/bcm/special/exit     -> default session
    // ------------------------------------------------------------------------
    server.on("/api/bcm/special", HTTP_GET, []() {
        char *responseBuffer = g_apiBufBig;
        MainWebServer::setLiveStream0200(false);
        char statusMessage[96];
        BcmSpecial::enter(statusMessage, sizeof(statusMessage));
        if (BcmSpecial::catalogJson(responseBuffer, sizeof(g_apiBufBig)) == 0) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"catalog too large\"}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/bcm/special/read", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        BcmSpecial::read(server.arg("key").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/bcm/special/run", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        int value = server.hasArg("value") ? server.arg("value").toInt() : 0;
        BcmSpecial::run(server.arg("key").c_str(), value, server.arg("confirm") == "1", responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/bcm/special/restore", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        BcmSpecial::restore(server.arg("key").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/bcm/special/boost", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        BcmSpecial::readBoost(responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/bcm/special/boost/apply", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        BcmSpecial::writeBoost(server.arg("value").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/bcm/special/boost/restore", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        BcmSpecial::restoreBoost(responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/bcm/special/tpms11", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        BcmSpecial::tpmsConfigRead(responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/bcm/special/tpms12/read", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        BcmSpecial::tpmsSensorRead(responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/bcm/special/tpms12/write", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        BcmSpecial::tpmsSensorWrite(server.arg("front").c_str(), server.arg("rear").c_str(), server.arg("confirm") == "1", responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });

    // No /api/bcm/special/vin/{read,write} - the BCM has no VIN DID (removed 2026-10-31, see bcm_special.cpp).

    server.on("/api/bcm/special/exit", HTTP_POST, []() {
        BcmSpecial::exitPage("page closed");
        server.send(200, "application/json", "{\"ok\":true}");
    });

    // ------------------------------------------------------------------------
    // ABS special functions (see abs_special.h). The catalog lists the FRONT and REAR bleed procedures as
    // type-9 guided procedures; they run through the shared /api/proc/* engine (proc ids abs_bleed_front /
    // abs_bleed_rear), so /api/abs/special/run has no native functions left.
    //     GET  /api/abs/special          -> open the page + function list (FRONT bleed, REAR bleed)
    //     POST /api/abs/special/exit     -> default session
    // ------------------------------------------------------------------------
    server.on("/api/abs/special", HTTP_GET, []() {
        char *responseBuffer = g_apiBufMed;
        MainWebServer::setLiveStream0200(false);
        char statusMessage[160];
        AbsSpecial::enter(statusMessage, sizeof(statusMessage));
        if (AbsSpecial::catalogJson(responseBuffer, sizeof(g_apiBufMed)) == 0) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"catalog too large\"}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/abs/special/run", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        int value = server.hasArg("value") ? server.arg("value").toInt() : 0;
        AbsSpecial::run(server.arg("key").c_str(), value, server.arg("confirm") == "1", responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/abs/special/exit", HTTP_POST, []() {
        AbsSpecial::exitPage("page closed");
        server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/abs/special/vin/read", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        AbsSpecial::readVin(responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/abs/special/vin/write", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        AbsSpecial::writeVin(server.arg("vin").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });

    // LHC special functions (LhcSpecial, src/modules/lhc/lhc_special.h/.cpp - same per-module shape as
    // ECM/ABS/BCM/SPDO/RAD). The only function here is the Cruise / Push-to-Talk grip side - exposed as a
    // single config option with two values (read / apply / restore), writing DID F1E8 on
    // BOTH hand controls (LHC 0x7E3 + RHC 0x7E5). option 0 = cruise LEFT/PTT RIGHT, option 1 = cruise RIGHT.
    server.on("/api/lhc/special", HTTP_GET, []() {
        MainWebServer::setLiveStream0200(false);
        char *responseBuffer = g_apiBufBig;
        LhcSpecial::enter(nullptr, 0);
        if (LhcSpecial::catalogJson(responseBuffer, sizeof(g_apiBufBig)) == 0) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"catalog too large\"}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/lhc/special/read", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        LhcSpecial::read(server.arg("key").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/lhc/special/run", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        int value = server.hasArg("value") ? server.arg("value").toInt() : 0;
        LhcSpecial::run(server.arg("key").c_str(), value, server.arg("confirm") == "1", responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/lhc/special/restore", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        LhcSpecial::restore(server.arg("key").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/lhc/special/exit", HTTP_POST, []() {
        LhcSpecial::exitPage("page closed");
        server.send(200, "application/json", "{\"ok\":true}");
    });

    // ------------------------------------------------------------------------
    // SPDO special functions (configuration options, see spdo_special.h)
    //     GET  /api/spdo/special          -> open the page + function list
    //     POST /api/spdo/special/read     -> key=<id>
    //     POST /api/spdo/special/run      -> key=<id>&value=<option index>[&confirm=1]
    //     POST /api/spdo/special/restore  -> key=<id>
    //     POST /api/spdo/special/exit     -> default session
    //     POST /api/spdo/special/profile  -> set=<n> (manual) or set=auto
    // ------------------------------------------------------------------------
    server.on("/api/spdo/special", HTTP_GET, []() {
        char *responseBuffer = g_apiBufBig;
        MainWebServer::setLiveStream0200(false);
        char statusMessage[96];
        SpdoSpecial::enter(statusMessage, sizeof(statusMessage));
        if (SpdoSpecial::catalogJson(responseBuffer, sizeof(g_apiBufBig)) == 0) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"catalog too large\"}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/spdo/special/read", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        SpdoSpecial::read(server.arg("key").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/spdo/special/run", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        int value = server.hasArg("value") ? server.arg("value").toInt() : 0;
        SpdoSpecial::run(server.arg("key").c_str(), value, server.arg("confirm") == "1", responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/spdo/special/restore", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        SpdoSpecial::restore(server.arg("key").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/spdo/special/exit", HTTP_POST, []() {
        SpdoSpecial::exitPage("page closed");
        server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/spdo/special/profile", HTTP_POST, []() {
        String set = server.arg("set");
        bool succeeded = (set == "auto") ? SpdoSpecial::setProfile(-1) : SpdoSpecial::setProfile(set.toInt());
        char *responseBuffer = g_apiBufBig;
        if (!succeeded || SpdoSpecial::catalogJson(responseBuffer, sizeof(g_apiBufBig)) == 0) { server.send(400, "application/json", "{\"ok\":false}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/spdo/special/vin/read", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        SpdoSpecial::readVin(responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/spdo/special/vin/write", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        SpdoSpecial::writeVin(server.arg("vin").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/spdo/special/idblocks", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        SpdoSpecial::readIdBlocks(responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/spdo/special/idblocks/write", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        SpdoSpecial::writeIdBlocks(server.arg("blk157").c_str(), server.arg("blk158").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });

    // ------------------------------------------------------------------------
    // RAD special functions (radio configuration options, see rad_special.h)
    //     GET  /api/rad/special          -> open the page + function list
    //     POST /api/rad/special/read     -> key=<id>
    //     POST /api/rad/special/run      -> key=<id>&value=<option index>[&confirm=1]
    //     POST /api/rad/special/restore  -> key=<id>
    //     POST /api/rad/special/exit     -> default session
    //     POST /api/rad/special/profile  -> set=<n> (manual) or set=auto
    // ------------------------------------------------------------------------
    server.on("/api/rad/special", HTTP_GET, []() {
        char *responseBuffer = g_apiBufBig;
        MainWebServer::setLiveStream0200(false);
        char statusMessage[96];
        RadSpecial::enter(statusMessage, sizeof(statusMessage));
        if (RadSpecial::catalogJson(responseBuffer, sizeof(g_apiBufBig)) == 0) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"catalog too large\"}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/rad/special/read", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        RadSpecial::read(server.arg("key").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/rad/special/run", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        int value = server.hasArg("value") ? server.arg("value").toInt() : 0;
        RadSpecial::run(server.arg("key").c_str(), value, server.arg("confirm") == "1", responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/rad/special/restore", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        RadSpecial::restore(server.arg("key").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/rad/special/exit", HTTP_POST, []() {
        RadSpecial::exitPage("page closed");
        server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/rad/special/profile", HTTP_POST, []() {
        String set = server.arg("set");
        bool succeeded = (set == "auto") ? RadSpecial::setProfile(-1) : RadSpecial::setProfile(set.toInt());
        char *responseBuffer = g_apiBufBig;
        if (!succeeded || RadSpecial::catalogJson(responseBuffer, sizeof(g_apiBufBig)) == 0) { server.send(400, "application/json", "{\"ok\":false}"); return; }
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/rad/special/vin/read", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        RadSpecial::readVin(responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });
    server.on("/api/rad/special/vin/write", HTTP_POST, []() {
        char *responseBuffer = g_apiBufSmall;
        RadSpecial::writeVin(server.arg("vin").c_str(), responseBuffer, sizeof(g_apiBufSmall));
        server.send(200, "application/json", responseBuffer);
    });

    // ------------------------------------------------------------------------
    // UI flags.
    //     GET  /api/flags                        -> { canlog_enabled, devtest_enabled }
    // ------------------------------------------------------------------------
    server.on("/api/flags", HTTP_GET, []() {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"canlog_enabled\":%s,\"devtest_enabled\":%s}",
                 settings.canlog_enabled ? "true" : "false", settings.devtest_enabled ? "true" : "false");
        server.send(200, "application/json", buf);
    });
    // DTC reference lookup: GET /api/dtc/lookup?code=P0107 -> { code, desc } from the 1,685-code DTC database.
    server.on("/api/dtc/lookup", HTTP_GET, []() {
        String code = server.arg("code");
        code.trim(); code.toUpperCase();
        const char *desc = lookupHDDtcDescription(code.c_str());
        char esc[160]; size_t q = 0;
        for (const char *p = desc; *p && q < sizeof(esc) - 2; p++) { if (*p == '"' || *p == '\\') esc[q++] = '\\'; esc[q++] = *p; }
        esc[q] = 0;
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"code\":\"%s\",\"desc\":\"%s\"}", code.c_str(), esc);
        server.send(200, "application/json", buf);
    });
    // Vehicle info (read-only): odometer in km + miles (from the SPDO 0x0200 live block) and VIN (from the ECM).
    // GET /api/vehicle/info -> { ok, km, mi, vin }. No writes - odometer adjustment is intentionally not offered.
    server.on("/api/vehicle/info", HTTP_GET, []() {
        float km = -1.0f;
        GenInputs0200 gi;
        if (UdsHandler::readGenInputs0200Synchronous(HD_ECU_SPEEDO, gi, 700) && gi.valid)
            for (uint8_t i = 0; i < gi.count; i++)
                if (strcmp(GEN_KEYS[gi.key[i]], "odometer_km") == 0) { km = gi.value[i]; break; }
        char vinbuf[256]; vinbuf[0] = 0;
        EcuInfo::vinJson(HD_ECU_ECM, vinbuf, sizeof(vinbuf));
        char vin[24] = "";
        const char *p = strstr(vinbuf, "\"vin\":\"");
        if (p) { p += 7; uint8_t j = 0; while (*p && *p != '"' && j < sizeof(vin) - 1) vin[j++] = *p++; vin[j] = 0; }
        char buf[256]; int w = snprintf(buf, sizeof(buf), "{\"ok\":true,");
        if (km >= 0) w += snprintf(buf + w, sizeof(buf) - w, "\"km\":%.1f,\"mi\":%.1f,", (double)km, (double)(km * 0.621371f));
        else         w += snprintf(buf + w, sizeof(buf) - w, "\"km\":null,\"mi\":null,");
        snprintf(buf + w, sizeof(buf) - w, "\"vin\":\"%s\"}", vin);
        server.send(200, "application/json", buf);
    });
    // Read-only current FOB assignment from the BCM (0x7E2): keyless/FOB status (22 0203) and the stored
    // serial for each slot (2E 0178 <slot> selects the slot, 22 0170 reads its 12-byte "Serial Number Block
    // 1-6"). No Security Access and no programming - the slot-select is an unsecured write done before
    // unlocking. GET /api/bcm/fob_status -> { ok, status, fob1, fob2 } (hex).
    server.on("/api/bcm/fob_status", HTTP_GET, []() {
        UdsRawResult r;
        char status[48] = "", s0[40] = "", s1[40] = "";
        bool unlocked = false, conf0 = false, conf1 = false;
        uint8_t ses[2] = {0x10, 0x03}; UdsHandler::rawTransaction(HD_ECU_BCM, ses, 2, r, 400);
        uint8_t idq[3] = {0x22, 0xF1, 0xEA}; UdsHandler::rawTransaction(HD_ECU_BCM, idq, 3, r, 400);
        // The FOB serials read only AFTER a BCM unlock.
        // Retry the unlock for the flaky WiFi bridge; a 0x0000 seed needs no key (handled in bcmUnlockRetry).
        unlocked = bcmUnlockRetry(3);
        uint8_t q203[3] = {0x22, 0x02, 0x03}; if (UdsHandler::rawTransaction(HD_ECU_BCM, q203, 3, r, 600)) bcmPayloadHex(r, status, sizeof(status));
        bcmReadDidConfirmed(0x01, 0x70, s0, sizeof(s0), &conf0);   // FOB 1 (read 2-3x, confirm)
        bcmReadDidConfirmed(0x01, 0x73, s1, sizeof(s1), &conf1);   // FOB 2 (read 2-3x, confirm)
        uint8_t def[2] = {0x10, 0x01}; UdsHandler::rawTransaction(HD_ECU_BCM, def, 2, r, 300);
        char buf[260];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"unlocked\":%s,\"status\":\"%s\",\"fob1\":\"%s\",\"fob2\":\"%s\",\"conf1\":%s,\"conf2\":%s}",
                 unlocked ? "true" : "false", status, s0, s1, conf0 ? "true" : "false", conf1 ? "true" : "false");
        server.send(200, "application/json", buf);
    });
    // Assign one key FOB slot per call. POST /api/bcm/fob_write?s1=<hex>  (FOB 1, DID 0170) and/or
    // &s2=<hex> (FOB 2, DID 0173) - serials with no separators. Each slot given runs its OWN complete,
    // independent pass (own identification reads, own unlock, own write+activate block, own read-back) -
    // run as two full passes, one after the other, each with its own operator hand-off (confirm, place THIS
    // fob near the bike).
    // The web UI calls this once per FOB (fob1Assign() for a single FOB; fob2Assign() calls it twice, FOB 1
    // then FOB 2, each after its own confirm+place-FOB popups) so the operator sees the same step-by-step
    // flow for either card. Giving only s1 or only s2 writes just that slot - if a later pass fails, an
    // earlier one that already completed stays programmed.
    //   22 F1EF/F190/F1EA/F193            identification + security-code reads (compatibility check)
    //   27 01 / 27 02                      SecurityAccess unlock (BCM algorithm)
    //   2E 0178 00  -> 22 0178           clear FOB-programming flag, read back
    //   2E 0170/73 <12-byte serial>       write the slot's serial (FOB 1 = DID 0170, FOB 2 = DID 0173)
    //   22 0200                           status
    //   2F 31 01 01                       routine start
    //   2E 0178 01  -> 22 0178           set flag, read back
    //   2F 31 01 08 (FOB1) / 10 (FOB2)    ACTIVATE this FOB slot (the activation bits are cumulative)
    //   22 0203                           status
    //   2E 0178 00  -> 22 0178           finalize (clear the flag again)
    server.on("/api/bcm/fob_write", HTTP_POST, []() {
        String a1 = server.arg("s1"), a2 = server.arg("s2");
        uint8_t b1[12], b2[12];
        uint8_t n1 = a1.length() ? bcmHexToBytes(a1.c_str(), b1, sizeof(b1)) : 0;
        uint8_t n2 = a2.length() ? bcmHexToBytes(a2.c_str(), b2, sizeof(b2)) : 0;
        char buf[260];
        if ((a1.length() && !n1) || (a2.length() && !n2) || (!a1.length() && !a2.length())) {
            snprintf(buf, sizeof(buf), "{\"ok\":false,\"msg\":\"serial must be an even number of hex digits (up to 12 bytes)\"}");
            server.send(200, "application/json", buf); return;
        }
        UdsRawResult r;
        uint8_t ses[2] = {0x10, 0x03}; UdsHandler::rawTransaction(HD_ECU_BCM, ses, 2, r, 400);
        // Identification + security-code reads used for the FOB-model/market compatibility
        // check (step 9 of the procedure). Informational: a short/failed reply here does not block the
        // assignment (this firmware has no FOB-family database to check them against), matching how "Read ECU
        // information" treats any one slow/missing identifier.
        uint8_t qF1EF[3] = {0x22, 0xF1, 0xEF}; UdsHandler::rawTransaction(HD_ECU_ECM, qF1EF, 3, r, 400);   // ECM id
        uint8_t qF190[3] = {0x22, 0xF1, 0x90}; UdsHandler::rawTransaction(HD_ECU_BCM, qF190, 3, r, 400);   // VIN
        uint8_t qF1EA[3] = {0x22, 0xF1, 0xEA}; UdsHandler::rawTransaction(HD_ECU_BCM, qF1EA, 3, r, 400);   // BCM id
        uint8_t qF193[3] = {0x22, 0xF1, 0x93}; UdsHandler::rawTransaction(HD_ECU_BCM, qF193, 3, r, 400);   // security code
        if (!bcmUnlockRetry(3)) {
            uint8_t def[2] = {0x10, 0x01}; UdsHandler::rawTransaction(HD_ECU_BCM, def, 2, r, 300);
            snprintf(buf, sizeof(buf), "{\"ok\":false,\"msg\":\"BCM unlock failed - check the link and retry\"}");
            server.send(200, "application/json", buf); return;
        }
        bool w1 = false, w2 = false;
        // failMsg1/2 record WHICH step of the pass actually failed, so the response message can point at the
        // real cause instead of one generic line.
        char failMsg1[96] = "", failMsg2[96] = "";
        // One FOB-slot pass: clear/read-back flag, write serial, status, routine start, set/read-back flag,
        // activate THIS slot (activateBit), status, finalize flag. didHi/didLo select DID 0170 (FOB 1) or
        // DID 0173 (FOB 2); activateBit is 0x08 for FOB 1, 0x10 for FOB 2 (cumulative).
        // IMPORTANT: a pass is only reported as successful if BOTH the serial write AND the ACTIVATE routine
        // (2F 31 01 nn) get a POSITIVE reply - earlier builds returned success as soon as the serial write
        // (2E 0170/0173) was accepted, ignoring the activate step's own result entirely. That let an
        // assignment report "assigned" even when the FOB was never actually near the antenna, because writing
        // the serial into NVRAM and activating the FOB's RF pairing are two separate UDS exchanges - the BCM
        // happily stores the serial bytes regardless of the FOB being present, but the ACTIVATE routine
        // (which is what actually performs/confirms the RF handshake - the FOB must stay within antenna
        // range through the activation step) can - and on real hardware did - come back negative
        // (NRC, most likely 0x22 "conditions not correct") when the FOB wasn't in range. That NRC is now
        // captured and is exactly what decides the real pass/fail result below.
        auto fobPass = [&](uint8_t didHi, uint8_t didLo, const uint8_t *serial, uint8_t serialLen, uint8_t activateBit,
                            char *failMsg, size_t failMsgLen) -> bool {
            uint8_t clrFlag[4] = {0x2E, 0x01, 0x78, 0x00}; UdsHandler::rawTransaction(HD_ECU_BCM, clrFlag, 4, r, 500);
            uint8_t rbFlag[3]  = {0x22, 0x01, 0x78}; UdsHandler::rawTransaction(HD_ECU_BCM, rbFlag, 3, r, 400);
            uint8_t cmd[3 + 12] = {0x2E, didHi, didLo}; memcpy(cmd + 3, serial, serialLen);
            UdsHandler::rawTransaction(HD_ECU_BCM, cmd, (uint8_t)(3 + serialLen), r, 700);
            bool wroteSerial = r.got_reply && r.positive;
            if (!wroteSerial) {
                if (r.got_reply) snprintf(failMsg, failMsgLen, "serial write rejected (NRC 0x%02X) - check the entered serial (24 hex digits)", r.nrc);
                else             snprintf(failMsg, failMsgLen, "serial write: no response from the BCM");
            }
            uint8_t status0200[3] = {0x22, 0x02, 0x00}; UdsHandler::rawTransaction(HD_ECU_BCM, status0200, 3, r, 400);
            uint8_t routineStart[4] = {0x2F, 0x31, 0x01, 0x01}; UdsHandler::rawTransaction(HD_ECU_BCM, routineStart, 4, r, 600);
            uint8_t setFlag[4] = {0x2E, 0x01, 0x78, 0x01}; UdsHandler::rawTransaction(HD_ECU_BCM, setFlag, 4, r, 500);
            UdsHandler::rawTransaction(HD_ECU_BCM, rbFlag, 3, r, 400);
            uint8_t activate[4] = {0x2F, 0x31, 0x01, activateBit};
            UdsHandler::rawTransaction(HD_ECU_BCM, activate, 4, r, 600);
            bool activateOk = r.got_reply && r.positive;
            if (wroteSerial && !activateOk) {
                if (r.got_reply) snprintf(failMsg, failMsgLen, "FOB activation rejected (NRC 0x%02X) - hold the FOB close to the motorcycle, within antenna range, through activation, and check its battery", r.nrc);
                else             snprintf(failMsg, failMsgLen, "FOB activation: no response from the BCM - hold the FOB close to the motorcycle, within antenna range, through activation");
                Serial.printf("[FOB] activate (2F 31 01 %02X) rejected/no reply, NRC 0x%02X\n", activateBit, r.nrc);
            }
            uint8_t status0203[3] = {0x22, 0x02, 0x03}; UdsHandler::rawTransaction(HD_ECU_BCM, status0203, 3, r, 400);
            UdsHandler::rawTransaction(HD_ECU_BCM, clrFlag, 4, r, 500);                 // finalize: clear the flag again
            UdsHandler::rawTransaction(HD_ECU_BCM, rbFlag, 3, r, 400);
            return wroteSerial && activateOk;
        };
        if (n1) w1 = fobPass(0x01, 0x70, b1, n1, 0x08, failMsg1, sizeof(failMsg1));     // FOB 1 pass (DID 0170, activate bit 08)
        if (n2) w2 = fobPass(0x01, 0x73, b2, n2, 0x10, failMsg2, sizeof(failMsg2));     // FOB 2 pass (DID 0173, activate bit 10)
        char s0[40] = "", s1[40] = ""; bool c0 = false, c1 = false;
        bcmReadDidConfirmed(0x01, 0x70, s0, sizeof(s0), &c0);                       // read back FOB 1
        bcmReadDidConfirmed(0x01, 0x73, s1, sizeof(s1), &c1);                       // read back FOB 2
        uint8_t def[2] = {0x10, 0x01}; UdsHandler::rawTransaction(HD_ECU_BCM, def, 2, r, 300);
        bool ok = (!n1 || w1) && (!n2 || w2);
        char msg[160];
        if (ok) {
            snprintf(msg, sizeof(msg), "New Key FOB was assigned");
        } else if (n1 && !w1 && failMsg1[0]) {
            snprintf(msg, sizeof(msg), "Programming FOB failed - %s", failMsg1);
        } else if (n2 && !w2 && failMsg2[0]) {
            snprintf(msg, sizeof(msg), "Programming FOB failed - %s", failMsg2);
        } else {
            snprintf(msg, sizeof(msg), "Programming FOB failed - check the read-back value, FOB range, and that it is close to the motorcycle");
        }
        char msgEsc[160]; jsonEscapeInto(msgEsc, sizeof(msgEsc), msg);
        snprintf(buf, sizeof(buf),
                 "{\"ok\":%s,\"fob1\":\"%s\",\"fob2\":\"%s\",\"conf1\":%s,\"conf2\":%s,\"msg\":\"%s\"}",
                 ok ? "true" : "false", s0, s1, c0 ? "true" : "false", c1 ? "true" : "false", msgEsc);
        server.send(200, "application/json", buf);
    });
    // Read the current 5-digit anti-theft PIN from the BCM (0x7E2, DID 0141). Unlocks only if the module
    // asks for it (a 0x0000 seed means no unlock - the key is NOT sent). GET /api/bcm/pin_read -> { ok, pin }.
    server.on("/api/bcm/pin_read", HTTP_GET, []() {
        UdsRawResult r; char pin[16] = ""; bool unlocked = false, conf = false;
        uint8_t ses[2] = {0x10, 0x03}; UdsHandler::rawTransaction(HD_ECU_BCM, ses, 2, r, 400);
        uint8_t idq[3] = {0x22, 0xF1, 0xEA}; UdsHandler::rawTransaction(HD_ECU_BCM, idq, 3, r, 400);
        unlocked = bcmUnlockRetry(3);                  // retry the unlock for the flaky bridge (0x0000 seed = no key)
        // DID 0141 payload = 5 PIN digit-bytes. Read it the reliable way (up to 6 tries, accept on 2 agreeing)
        // and decode from the confirmed hex string, so a single dropped frame no longer shows a read error.
        char hex[40] = "";
        bcmReadDidConfirmed(0x01, 0x41, hex, sizeof(hex), &conf);
        size_t q = 0;
        for (size_t i = 0; hex[i] && hex[i + 1] && q + 1 < sizeof(pin) && q < 5; i += 2) {
            auto nib = [](char c) -> int { return (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1; };
            int hi = nib(hex[i]), lo = nib(hex[i + 1]);
            uint8_t b = (uint8_t)((hi < 0 ? 0 : hi) << 4 | (lo < 0 ? 0 : lo));
            pin[q++] = (b <= 9) ? (char)('0' + b) : ((b >= 0x20 && b < 0x7F) ? (char)b : '?');
        }
        pin[q] = 0;
        uint8_t def[2] = {0x10, 0x01}; UdsHandler::rawTransaction(HD_ECU_BCM, def, 2, r, 300);
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"unlocked\":%s,\"conf\":%s,\"pin\":\"%s\"}",
                 unlocked ? "true" : "false", conf ? "true" : "false", pin);
        server.send(200, "application/json", buf);
    });
    // Generic guided-procedure engine (shared by every module's interactive special functions).
    //   POST /api/proc/start?id=<procId>                 -> open + advance to the first prompt
    //   POST /api/proc/step?answer=<0|1>&input=<hex/str> -> advance one operator step
    //   POST /api/proc/abort                             -> stop + return the module to default session
    server.on("/api/proc/start", HTTP_POST, []() {
        char *buf = g_apiBufSmall;
        MainWebServer::setLiveStream0200(false);
        ProcRunner::start(ProcRunner::find(server.arg("id").c_str()), buf, sizeof(g_apiBufSmall));
        server.send(200, "application/json", buf);
    });
    server.on("/api/proc/step", HTTP_POST, []() {
        char *buf = g_apiBufSmall;
        int answer = server.hasArg("answer") ? server.arg("answer").toInt() : 1;
        String input = server.arg("input");
        ProcRunner::step(answer, input.length() ? input.c_str() : nullptr, buf, sizeof(g_apiBufSmall));
        server.send(200, "application/json", buf);
    });
    server.on("/api/proc/abort", HTTP_POST, []() {
        char *buf = g_apiBufSmall;
        ProcRunner::abort("operator stop", buf, sizeof(g_apiBufSmall));
        server.send(200, "application/json", buf);
    });
    // Live master-cylinder pressure for the pressure gate / on-screen readout (polled while a popup waits).
    server.on("/api/proc/live", HTTP_GET, []() {
        char *buf = g_apiBufSmall;
        ProcRunner::liveJson(buf, sizeof(g_apiBufSmall));
        server.send(200, "application/json", buf);
    });
    server.on("/api/settings/canlog", HTTP_POST, []() {
        bool en = (server.arg("enabled") == "1" || server.arg("enabled") == "true");
        settings.canlog_enabled = en;
        Preferences prefs;
        prefs.begin("hd_cfg", false);
        prefs.putBool("canlog_en", en);
        prefs.end();
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"canlog_enabled\":%s}", en ? "true" : "false");
        server.send(200, "application/json", buf);
    });
    // Developer test mode: shows "Continue (debug)" on guided-procedure brake-pressure gates (ABS bleed
    // press/release/pumpwait) so the operator can skip the live pressure check. Off by default, persisted to
    // NVS like every other UI flag here, so a factory NVS reset also clears it back to off.
    server.on("/api/settings/devtest", HTTP_POST, []() {
        bool en = (server.arg("enabled") == "1" || server.arg("enabled") == "true");
        settings.devtest_enabled = en;
        Preferences prefs;
        prefs.begin("hd_cfg", false);
        prefs.putBool("devtest_en", en);
        prefs.end();
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"devtest_enabled\":%s}", en ? "true" : "false");
        server.send(200, "application/json", buf);
    });
    server.on("/api/uds/special", HTTP_POST, []() {
        uint32_t target_id = settings.active_module_id != 0 ? settings.active_module_id : 0x7E0;
        char json[380];
        snprintf(
            json,
            sizeof(json),
            "{\"ok\":true,\"operation\":\"Special Functions\",\"module_id\":\"0x%03X\",\"module_name\":\"%s\","
            "\"status\":\"Placeholder\",\"message\":\"Special Functions interface ready. HD key fob pairing, ABS brake bleed, and calibration routines will be integrated in future release.\"}",
            target_id,
            lookupModuleName(target_id)
        );
        server.send(200, "application/json", json);
    });

    // ------------------------------------------------------------------------
    // 9. Standard Custom UDS Execution & Direct Query
    // ------------------------------------------------------------------------
    server.on("/api/uds", HTTP_POST, []() {
        // Numbers accept decimal or 0x-prefixed hex (strtoul base 0); all fields are range-checked.
        auto argU = [](const char *name, unsigned long def) -> unsigned long {
            if (!server.hasArg(name)) return def;
            String v = server.arg(name);
            v.trim();
            if (v.length() == 0) return def;
            char *end = nullptr;
            unsigned long r = strtoul(v.c_str(), &end, 0);
            return (end == v.c_str()) ? def : r;
        };
        unsigned long target_id = argU("target_id", settings.active_module_id != 0 ? settings.active_module_id : 0x7E0);
        unsigned long sid       = argU("sid", 0x22);
        unsigned long sub       = argU("sub", 0xF1);
        unsigned long did_low   = argU("did_low", 0xEF);
        String        raw       = server.hasArg("raw") ? server.arg("raw") : "";

        if (live_stream_0200_enabled) {
            server.send(409, "application/json", "{\"tx_ok\":false,\"error\":\"live DID 0200 stream active - stop it first\"}");
            return;
        }

        if (target_id < HD_MIN_DIAG_ID || target_id > 0x7FF || sid > 0xFF || sub > 0xFF || did_low > 0xFF || raw.length() > 48) {
            server.send(400, "application/json", "{\"tx_ok\":false,\"error\":\"invalid parameter\"}");
            return;
        }

        bool tx_ok = UdsHandler::sendDiagnosticRequest((uint32_t)target_id, (uint8_t)sid, (uint8_t)sub, (uint8_t)did_low, raw.c_str());
        server.send(200, "application/json", tx_ok ? "{\"tx_ok\":true}" : "{\"tx_ok\":false}");
    });

    server.on("/api/uds/result", HTTP_GET, []() {
        static char s_asciiEsc[400];
        static char s_decodedEsc[400];
        static char s_resultJson[1100];
        UdsPendingTransaction pendingTransaction = UdsHandler::getLatestResult();
        char hexData[32] = {0};
        if (pendingTransaction.completed) {
            char *p = hexData;
            for (uint8_t i = 0; i < pendingTransaction.rx_dlc && i < 8; i++) {
                p += sprintf(p, i == 0 ? "%02X" : " %02X", pendingTransaction.rx_data[i]);
            }
        }
        jsonEscapeInto(s_asciiEsc, sizeof(s_asciiEsc), pendingTransaction.ascii_payload);
        jsonEscapeInto(s_decodedEsc, sizeof(s_decodedEsc), pendingTransaction.decoded);
        snprintf(
            s_resultJson,
            sizeof(s_resultJson),
            "{\"pending\":%s,\"completed\":%s,\"is_negative\":%s,\"rx_id\":\"0x%03X\",\"elapsed_ms\":%u,\"data\":\"%s\",\"ascii\":\"%s\",\"decoded\":\"%s\"}",
            pendingTransaction.pending ? "true" : "false",
            pendingTransaction.completed ? "true" : "false",
            pendingTransaction.is_negative ? "true" : "false",
            (unsigned)pendingTransaction.rx_id,
            (unsigned)pendingTransaction.elapsed_ms,
            hexData,
            s_asciiEsc,
            s_decodedEsc
        );
        server.send(200, "application/json", s_resultJson);
    });

    // ------------------------------------------------------------------------
    // 10. Device Configuration, SafeBoot, Reboot & OTA
    // ------------------------------------------------------------------------
    server.on("/api/config", HTTP_GET, []() {
        // Passwords are never sent back to the browser (write-only fields).
        char nn[80], ws[80], as[80];
        jsonEscapeInto(nn, sizeof(nn), settings.node_name);
        jsonEscapeInto(ws, sizeof(ws), settings.wifi_ssid);
        jsonEscapeInto(as, sizeof(as), settings.ap_ssid);
        const char *canChipStr = (settings.can_chip_type == CAN_CHIP_TJA1043) ? "tja1043" : "plain";
        char json[700];
        snprintf(
            json,
            sizeof(json),
            "{\"node_name\":\"%s\",\"wifi_mode\":\"%s\",\"wifi_ssid\":\"%s\",\"wifi_pass\":\"\","
            "\"ap_ssid\":\"%s\",\"ap_pass\":\"\",\"ap_secured\":%s,\"can_bitrate\":%u,"
            "\"active_module_id\":%u,\"mac_addr\":\"%s\","
            "\"can_chip\":\"%s\",\"can_tx_pin\":%u,\"can_rx_pin\":%u,"
            "\"tja_stb_pin\":%u,\"tja_en_pin\":%u,\"tja_err_pin\":%u,\"tja_led_pin\":%u}",
            nn,
            settings.wifi_ap_mode ? "AP" : "STA",
            ws,
            as,
            strlen(settings.ap_pass) >= 8 ? "true" : "false",
            (unsigned)settings.can_bitrate,
            (unsigned)settings.active_module_id,
            WiFi.macAddress().c_str(),
            canChipStr,
            (unsigned)settings.can_tx_pin,
            (unsigned)settings.can_rx_pin,
            (unsigned)settings.tja_stb_pin,
            (unsigned)settings.tja_en_pin,
            (unsigned)settings.tja_err_pin,
            (unsigned)settings.tja_led_pin
        );
        server.send(200, "application/json", json);
    });

    server.on("/api/config", HTTP_POST, []() {
        Serial.println("[WEB] POST /api/config -> Validating and updating NVS configuration...");
        // ---- validate first, write only if everything is acceptable ----
        if (server.hasArg("node_name") && (server.arg("node_name").length() < 1 || server.arg("node_name").length() > 31)) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"node_name length 1..31\"}"); return;
        }
        if (server.hasArg("ap_ssid") && (server.arg("ap_ssid").length() < 1 || server.arg("ap_ssid").length() > 32)) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"ap_ssid length 1..32\"}"); return;
        }
        if (server.hasArg("ap_pass") && server.arg("ap_pass").length() != 0 &&
            (server.arg("ap_pass").length() < 8 || server.arg("ap_pass").length() > 63)) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"ap_pass length 8..63 (or empty for open AP)\"}"); return;
        }
        if (server.hasArg("wifi_ssid") && server.arg("wifi_ssid").length() > 32) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"wifi_ssid too long\"}"); return;
        }
        if (server.hasArg("wifi_pass") && server.arg("wifi_pass").length() > 63) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"wifi_pass too long\"}"); return;
        }
        uint32_t newBitrate = 0;
        if (server.hasArg("can_bitrate")) {
            newBitrate = (uint32_t)server.arg("can_bitrate").toInt();
            if (newBitrate != 125000 && newBitrate != 250000 && newBitrate != 500000 && newBitrate != 1000000) {
                server.send(400, "application/json", "{\"ok\":false,\"error\":\"can_bitrate must be 125000/250000/500000/1000000\"}"); return;
            }
        }

        // ---- CAN transceiver chip type + GPIO pin assignment validation ----
        int newCanChipType = -1;
        if (server.hasArg("can_chip")) {
            String cc = server.arg("can_chip");
            if (cc == "tja1043" || cc == "0") newCanChipType = CAN_CHIP_TJA1043;
            else if (cc == "plain" || cc == "1") newCanChipType = CAN_CHIP_PLAIN;
            else {
                server.send(400, "application/json", "{\"ok\":false,\"error\":\"can_chip must be 'tja1043' or 'plain'\"}"); return;
            }
        }

        // Each entry: {form arg name, current value, parsed-new-value pointer}
        struct PinField { const char *argName; uint8_t currentValue; };
        // NOTE: the CAN-activity LED (GPIO 2) and status LED (GPIO 15) are hardwired in
        // firmware and intentionally NOT part of this configurable set.
        PinField pinFields[5] = {
            { "can_tx_pin",  settings.can_tx_pin  },
            { "can_rx_pin",  settings.can_rx_pin  },
            { "tja_stb_pin", settings.tja_stb_pin },
            { "tja_en_pin",  settings.tja_en_pin  },
            { "tja_err_pin", settings.tja_err_pin },
        };
        uint8_t effectivePins[5];
        bool    pinSubmitted[5];
        for (int i = 0; i < 5; i++) {
            if (server.hasArg(pinFields[i].argName)) {
                long parsedPin = server.arg(pinFields[i].argName).toInt();
                if (parsedPin < 0 || parsedPin > 39) {
                    server.send(400, "application/json", "{\"ok\":false,\"error\":\"GPIO pin out of range (0-39)\"}"); return;
                }
                if (parsedPin >= 6 && parsedPin <= 11) {
                    server.send(400, "application/json", "{\"ok\":false,\"error\":\"GPIO 6-11 are reserved for internal flash and cannot be used\"}"); return;
                }
                if (parsedPin == (long)SAFEBOOT_BUTTON_PIN || parsedPin == (long)STATUS_LED_PIN || parsedPin == (long)CAN_ACTIVITY_LED_PIN) {
                    server.send(400, "application/json", "{\"ok\":false,\"error\":\"GPIO pin collides with a fixed system pin (Safe Boot button, status LED or CAN activity LED)\"}"); return;
                }
                effectivePins[i] = (uint8_t)parsedPin;
                pinSubmitted[i]  = true;
            } else {
                effectivePins[i] = pinFields[i].currentValue;
                pinSubmitted[i]  = false;
            }
        }
        // Reject duplicate GPIO assignments among the resulting (submitted + unchanged) CAN pin set
        for (int i = 0; i < 5; i++) {
            for (int j = i + 1; j < 5; j++) {
                if (effectivePins[i] == effectivePins[j]) {
                    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Duplicate GPIO pin assigned to more than one CAN interface signal\"}"); return;
                }
            }
        }

        Preferences prefs;
        prefs.begin("hd_cfg", false);
        if (server.hasArg("node_name")) prefs.putString("node_name", server.arg("node_name"));
        if (server.hasArg("wifi_mode")) {
            String wm = server.arg("wifi_mode");
            prefs.putBool("ap_mode", (wm == "AP" || wm == "0"));
        }
        if (server.hasArg("ap_ssid")) prefs.putString("ap_ssid", server.arg("ap_ssid"));
        if (server.hasArg("ap_pass")) prefs.putString("ap_pass", server.arg("ap_pass"));
        if (server.hasArg("wifi_ssid")) prefs.putString("ssid", server.arg("wifi_ssid"));
        if (server.hasArg("wifi_pass")) prefs.putString("pass", server.arg("wifi_pass"));
        if (newBitrate) prefs.putUInt("bitrate", newBitrate);
        if (newCanChipType >= 0) prefs.putUChar("can_chip", (uint8_t)newCanChipType);
        if (pinSubmitted[0]) prefs.putUChar("can_tx_pin", effectivePins[0]);
        if (pinSubmitted[1]) prefs.putUChar("can_rx_pin", effectivePins[1]);
        if (pinSubmitted[2]) prefs.putUChar("tja_stb_pin", effectivePins[2]);
        if (pinSubmitted[3]) prefs.putUChar("tja_en_pin", effectivePins[3]);
        if (pinSubmitted[4]) prefs.putUChar("tja_err_pin", effectivePins[4]);
        prefs.end();
        server.send(200, "application/json", "{\"ok\":true}");
        delay(300);
        ESP.restart();
    });

    auto handleSystemReboot = []() {
        Serial.println("[WEB] System Reboot requested via Web UI. Restarting ESP32...");
        server.send(200, "application/json", "{\"ok\":true}");
        delay(250);
        ESP.restart();
    };
    server.on("/api/reboot", HTTP_POST, handleSystemReboot);
    server.on("/api/system/reboot", HTTP_POST, handleSystemReboot);

    auto handleSystemSafeBoot = []() {
        Serial.println("[WEB] Safe Boot Recovery Mode requested via Web UI.");
        server.send(200, "application/json", "{\"ok\":true}");
        delay(200);
        SafeBoot::enableAndReboot();
    };
    server.on("/api/safeboot", HTTP_POST, handleSystemSafeBoot);
    server.on("/api/system/safeboot", HTTP_POST, handleSystemSafeBoot);

    auto handleSystemFactoryReset = []() {
        Serial.println("[WEB] Factory Reset requested via Web UI. Erasing NVS flash...");
        Preferences prefs;
        prefs.begin("hd_cfg", false);
        prefs.clear();
        prefs.end();
        nvs_flash_deinit();
        nvs_flash_erase();
        nvs_flash_init();
        server.send(200, "application/json", "{\"ok\":true}");
        delay(300);
        ESP.restart();
    };
    server.on("/api/factory_reset", HTTP_POST, handleSystemFactoryReset);
    server.on("/api/system/factory_reset", HTTP_POST, handleSystemFactoryReset);

    server.on(
        "/api/ota/upload",
        HTTP_POST,
        []() {
            if (OtaHelper::respond(server)) {
                delay(500);
                ESP.restart();
            }
        },
        []() { OtaHelper::feed(server.upload(), "OTA"); }
    );

    if (s_stream_mutex == nullptr) s_stream_mutex = xSemaphoreCreateMutex();
    if (s_poll_task_handle == nullptr) {
        xTaskCreatePinnedToCore(liveStreamPollTask, "udsPoll", 8192, nullptr, 2, &s_poll_task_handle, 1);
    }
    server.begin();
    Serial.printf("[WEB] %s v%s #%s (%s) HTTP Server started on port 80\n",
                  APP_NAME_STR, FIRMWARE_VERSION_STR, FIRMWARE_BUILD_NUM_STR, APP_COPYRIGHT_STR);
}
