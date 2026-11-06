/**
 * ============================================================================
 * File     : src/core/web_server.h
 * Project  : ESP32 HD - UDS Diagnostic Service Tester (ISO 14229 / ISO 15765-2)
 * Copyright: (c) 2026 Henzulea Daniel email: <zulea1@gmail.com>
 * License  : GNU General Public License v3.0 or later (GPL-3.0-or-later) - see LICENSE
 * Version   : see FIRMWARE_VERSION_STR / FIRMWARE_BUILD_NUM_STR in src/config.h
 * ============================================================================
 * DESCRIPTION:
 *   HTTP & WebSocket Diagnostic Server interface running on Core 1 (Port 80).
 *   Serves the embedded HD Black/Dark-Grey/Orange Web UI from
 *   PROGMEM using chunked zero-heap transfers, streams real-time ECM Live
 *   Module Inputs (DID 0x0200, 41 parameters), ABS Live Sensor Inputs
 *   (DID 0x0200, 13 parameters), and BCM Live Body & Security Inputs
 *   (DID 0x0200, 12 parameters) over WebSocket (`/ws`), and exposes WebSocket
 *   & REST endpoints for:
 *     - UDS Broadcast Module Discovery (0x7DF -> 0x7E0..0x7F2)
 *     - Module Selection & Background TesterPresent (0x3E 0x00 / 0x3E 0x80) Control
 *     - Sequential & Individual DID Identification Queries (Service 0x22)
 *     - ECM, ABS & BCM Read Module Inputs Live Telemetry (DID 0x0200)
 *     - ECM Set Module Outputs Actuator Control (Service 0x2F, 7 DIDs,
 *       non-blocking pulse timers, <11.8V battery safeguard, failsafe cleanup)
 *     - ABS Set Module Outputs Actuator Control (Service 0x2F on 0x7E1,
 *       Extended Session 02 10 03, 2000ms TesterPresent 02 3E 80, DID 0x0540
 *       1s Pump Motor Pulse, Wheel Speed > 0 Safety Lock, failsafe cleanup)
 *     - Diagnostic Trouble Code (DTC) Read & Clear (Service 0x19 / 0x14)
 *     - Device NVS Configuration, Safe Boot Trigger, Factory Reset, and OTA
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include "uds_handler.h"

class MainWebServer {
public:
    /**
     * @brief Registers all HTTP REST endpoints, WebSocket `/ws` upgrade handler,
     *        and starts the server on port 80.
     */
    static void init();

    /**
     * @brief Processes incoming HTTP requests, WebSocket frames, non-blocking
     *        actuator pulse timers, <11.8V battery voltage monitor, and active
     *        DID 0x0200 live telemetry streaming in the Core 1 main loop.
     */
    static void handleClient();

    /**
     * @brief Serializes all 41 extracted parameters from DID 0x0200 into a
     *        compact JSON payload:
     *        {"type":"live_data","module":"ecm","did":"0200","data":{...}}
     */
    static size_t serializeEcmInputs0200Json(const EcmInputs0200 &ecmInputs, char *outputJsonBuffer, size_t maxJsonLength, bool includeOkFlag = false);

    /**
     * @brief Broadcasts the serialized ECM DID 0x0200 live telemetry JSON payload
     *        to all connected WebSocket clients on `/ws`.
     */
    static void broadcastEcmInputs0200(const EcmInputs0200 &ecmInputs);

    /**
     * @brief Serializes all 13 extracted ABS parameters from DID 0x0200 (0x7E1)
     *        into a compact JSON payload:
     *        {"type":"live_data","module":"abs","did":"0200","data":{...}}
     */
    static size_t serializeAbsInputs0200Json(const AbsInputs0200 &absInputs, char *outputJsonBuffer, size_t maxJsonLength, bool includeOkFlag = false);

    /**
     * @brief Broadcasts the serialized ABS DID 0x0200 live telemetry JSON payload
     *        to all connected WebSocket clients on `/ws`.
     */
    static void broadcastAbsInputs0200(const AbsInputs0200 &absInputs);

    /**
     * @brief Serializes all 12 extracted BCM parameters from DID 0x0200 (0x7E2)
     *        into a compact JSON payload:
     *        {"type":"live_data","module":"bcm","did":"0200","data":{...}}
     */
    static size_t serializeBcmInputs0200Json(const BcmInputs0200 &bcmInputs, char *outputJsonBuffer, size_t maxJsonLength, bool includeOkFlag = false);

    /**
     * @brief Broadcasts the serialized BCM DID 0x0200 live telemetry JSON payload
     *        to all connected WebSocket clients on `/ws`.
     */
    static void broadcastBcmInputs0200(const BcmInputs0200 &bcmInputs);
    static size_t serializeGenInputs0200Json(const GenInputs0200 &genInputs, char *outputJsonBuffer, size_t maxJsonLength, bool includeOkFlag = false);
    static void broadcastGenInputs0200(const GenInputs0200 &genInputs);

    /**
     * @brief Broadcasts an arbitrary JSON text message to all connected `/ws` clients.
     */
    static void broadcastWsJson(const char *jsonPayload, size_t payloadLength = 0);

    /**
     * @brief Processes an incoming WebSocket JSON command (such as
     *        `{"action":"control_output","did":"0502","state":1}` or
     *        `{"action":"cleanup_outputs"}`).
     */
    static void processWsActionJson(const char *messageBuffer);

    /**
     * @brief Enables or disables periodic live streaming of ECM DID 0x0200.
     */
    static void setLiveStream0200(bool enableStreaming);

    /**
     * @brief Optional hook for CAN bus frame logging.
     */
    static void logTraffic(uint32_t canIdentifier, bool isExtendedFrame, bool isRemoteFrame, uint8_t dataLengthCode, const uint8_t *frameDataBytes, const char *direction) {}
};

