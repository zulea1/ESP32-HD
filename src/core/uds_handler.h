/**
 * ============================================================================
 * File      : src/core/uds_handler.h
 * Project   : ESP32 HD - UDS Diagnostic Service Tester (ISO 14229 / ISO 15765-2)
 * Copyright : (c) 2026 Henzulea Daniel email: <zulea1@gmail.com>
 * License   : GNU General Public License v3.0 or later (GPL-3.0-or-later) - see LICENSE
 * Version   : see FIRMWARE_VERSION_STR / FIRMWARE_BUILD_NUM_STR in src/config.h
 * ============================================================================
 * OVERVIEW:
 * Implements ISO 14229 Unified Diagnostic Services (UDS) and ISO 15765-2
 * (ISO-TP) transport layer segmentation/reassembly for HD ECUs:
 *   - Multi-frame First Frame (0x10..0x1F), Flow Control (0x30 CTS), and
 *     Consecutive Frame (0x20..0x2F) reassembly with sequence verification.
 *   - Synchronous DID reading (Service 0x22) with automatic retry and
 *     background TesterPresent (0x3E) suppression during active sequences.
 *   - Dynamic Multi-Profile 41-parameter ECM Live Data parser for DID 0x0200
 *     (0x7E0) supporting 4 HD ECU generations (Map_M8_0200, Map_TC_ETC_0200,
 *     Map_TC_CABLE_0200, Map_XL_0200) latched at runtime via DID 0xF193 / 0xF1ED,
 *     13-parameter ABS Live Sensor Inputs parser for DID 0x0200 (0x7E1), and
 *     12-parameter BCM Live Body & Security Inputs parser for DID 0x0200 (0x7E2).
 *   - UDS Service 0x2F (InputOutputControlByIdentifier) actuator test engine
 *     for ECM (0x7E0, 7 DIDs) and ABS (0x7E1, DID 0x0540 Hydraulic Pump Motor
 *     1000ms pulse, Extended Session 02 10 03, 2000ms TesterPresent 02 3E 80,
 *     wheel speed km/h > 0 safety lock, NRC 03 7F 2F [NRC] sniffer, and
 *     failsafe cleanup 04 2F 05 40 00 + 02 10 01).
 *   - Broadcast ECU discovery (0x7DF), DTC reading (0x19), and clearing (0x14).
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include <driver/twai.h>
#include "config.h"

/**
 * @brief Tracks an active UDS request/response transaction across Core 0
 *        (CAN RX worker) and Core 1 (HTTP/WebSocket server). Protected by `udsMux`.
 */
struct UdsPendingTransaction {
    bool     pending;
    bool     completed;
    bool     is_negative;
    bool     isotp_multi;
    uint32_t req_id;
    uint32_t expected_rx_id;
    uint32_t rx_id;
    uint8_t  sid;
    uint16_t target_did;
    uint32_t start_ms;
    uint32_t elapsed_ms;
    uint8_t  rx_dlc;
    uint8_t  rx_data[8];
    uint16_t expected_len;
    uint16_t collected_len;
    uint8_t  cf_count;
    uint8_t  expected_sn;
    uint8_t  full_payload[128];
    char     ascii_payload[248];
    char     decoded[320];
};

/**
 * @brief Decoded HD Diagnostic Trouble Code (DTC) record.
 */
struct HDDtcRecord {
    char    code[16];        // e.g. "B2206-00" or "U0141-01"
    char    raw_hex[10];     // e.g. "920600"
    uint8_t status_byte;     // e.g. 0x08 (Confirmed), 0x88 (Active + Confirmed)
    char    status_desc[32]; // e.g. "Confirmed Fault (0x08)"
    char    description[64]; // e.g. "Engine Stop / Run Switch Input Fault"
};

/**
 * @brief Result & state snapshot for UDS Service 0x2F (InputOutputControlByIdentifier)
 *        actuator commands on ECM (0x7E0) and ABS (0x7E1).
 */
struct ActuatorOutputResult {
    bool     tx_ok;
    bool     got_reply;
    bool     is_positive;       // true if ECU replied with 0x6F Positive Response
    bool     is_negative;       // true if ECU replied with 0x7F Negative Response (NRC)
    bool     rpm_interlock;     // true if blocked by Engine Safety Interlock (RPM > 0)
    bool     speed_interlock;   // true if blocked by ABS Wheel Speed Safety Interlock (km/h > 0)
    uint8_t  nrc;               // UDS Negative Response Code (e.g. 0x22 ConditionsNotCorrect)
    uint16_t did;               // Actuator DID (0x0501..0x0506, 0x0530, 0x0540)
    uint8_t  requested_state;   // 1 = ON/Pulse, 0 = OFF, 2 = ReturnControlToECU (0x00)
    uint8_t  current_state;     // 0 = IDLE/OFF, 1 = ON, 2 = PULSING
    uint32_t pulse_duration_ms; // >0 when non-blocking millis() auto-reset timer is armed
    char     tx_frame_hex[32];  // Transmitted 8-byte CAN frame hex
    char     rx_frame_hex[32];  // Received 8-byte CAN response hex
    char     message[128];      // Diagnostic confirmation or NRC description
};

/**
 * @brief Extracted 41-parameter dynamic telemetry structure for HD ECM
 *        DID 0x0200 ("Read Module Inputs" Value + State Live Data across M8, TC_ETC,
 *        TC_CABLE, and XL profiles).
 */
struct EcmInputs0200 {
    bool      valid;
    uint16_t  raw_len;
    uint32_t  timestamp_ms;
    EcuFamily ecu_family;

    // Indexed dynamic storage for all 41 TOTAL_PARAM_COUNT parameters
    float param_values[TOTAL_PARAM_COUNT];
    bool  param_is_na[TOTAL_PARAM_COUNT];
    bool  param_is_string[TOTAL_PARAM_COUNT];
    char  param_strings[TOTAL_PARAM_COUNT][24];

    // 41 Direct Dynamic Properties bound to DynamicParamMap::json_key
    float battery_voltage;               // IDX_BATT_VOLT       ("battery_voltage")
    float map_kpa;                       // IDX_MAP_KPA         ("map_kpa")
    float map_volts;                     // IDX_MAP_VOLTS       ("map_volts")
    float engine_temperature;            // IDX_ENGINE_TEMP     ("engine_temperature")
    float engine_temperature_sensor;     // IDX_ENGINE_TEMP_V   ("engine_temperature_sensor")
    float intake_air_temperature;        // IDX_IAT_DEG         ("intake_air_temperature")
    float intake_air_temperature_sensor; // IDX_IAT_VOLTS       ("intake_air_temperature_sensor")
    float throttle_position;             // IDX_THROTTLE_PCT    ("throttle_position")
    float throttle_position_sensor_1;    // IDX_VALID_TPS_V1    ("throttle_position_sensor_1")
    float throttle_position_sensor_2;    // IDX_VALID_TPS_V2    ("throttle_position_sensor_2")
    float twistgrip_position;            // IDX_TWIST_PCT       ("twistgrip_position")
    float twistgrip_sensor_1;            // IDX_TWIST_V1        ("twistgrip_sensor_1")
    float twistgrip_sensor_2;            // IDX_TWIST_V2        ("twistgrip_sensor_2")
    float idle_set_speed;                // IDX_IDLE_SET_RPM    ("idle_set_speed")
    float engine_speed;                  // IDX_RPM             ("engine_speed")
    float vehicle_speed;                 // IDX_SPEED           ("vehicle_speed")
    float front_closed_loop_integrator;  // IDX_FL_INT          ("front_closed_loop_integrator")
    float rear_closed_loop_integrator;   // IDX_RL_INT          ("rear_closed_loop_integrator")
    float front_adaptive_fuel_value;     // IDX_FL_AFV          ("front_adaptive_fuel_value")
    float rear_adaptive_fuel_value;      // IDX_RL_AFV          ("rear_adaptive_fuel_value")
    float front_oxygen_sensor;           // IDX_O2_FRONT        ("front_oxygen_sensor")
    float rear_oxygen_sensor;            // IDX_O2_REAR         ("rear_oxygen_sensor")
    float injector_time_front;           // IDX_INJ_F           ("injector_time_front")
    float injector_time_rear;            // IDX_INJ_R           ("injector_time_rear")
    float spark_advance_front;            // IDX_SPARK_F         ("spark_advance_front")
    float spark_advance_rear;             // IDX_SPARK_R         ("spark_advance_rear")
    float front_spark_knock_retard;      // IDX_KNOCK_F         ("front_spark_knock_retard")
    float rear_spark_knock_retard;       // IDX_KNOCK_R         ("rear_spark_knock_retard")
    float idle_air_control_steps;        // IDX_IAC_STEPS       ("idle_air_control_steps")

    char  run_mode_flag[16];             // IDX_FLAG_RUN        ("run_mode_flag": "On" / "Off")
    char  tssm_security_state[16];       // IDX_FLAG_SECURITY   ("tssm_security_state": "Armed" / "Disarmed")
    char  bank_angle_sensor_mode[16];    // IDX_FLAG_BANK_ANGLE ("bank_angle_sensor_mode": "Tipped" / "Run Mode")
    char  neutral_switch_input[16];      // IDX_FLAG_NEUTRAL    ("neutral_switch_input": "In Neutral" / "In Gear")
    char  clutch_lever_switch_state[16]; // IDX_FLAG_CLUTCH     ("clutch_lever_switch_state": "Pressed" / "Released")
    char  gear_position[16];             // IDX_GEAR_POSITION   ("gear_position": "Neutral" / "Gear N")
    char  cruise_brake_input[12];        // IDX_CRUISE_BRAKE    ("cruise_brake_input": "on" / "off" / "N/A")
    char  cruise_set[12];                // IDX_CRUISE_SET      ("cruise_set": "on" / "off" / "N/A")
    char  cruise_res[12];                // IDX_CRUISE_RES      ("cruise_res": "on" / "off" / "N/A")
    char  cruise_enable_switch[12];      // IDX_CRUISE_ENABLE   ("cruise_enable_switch": "on" / "off" / "N/A")
    char  cruise_mode[12];               // IDX_CRUISE_MODE     ("cruise_mode": "on" / "off" / "N/A")
    char  cruise_rolloff_input[12];      // IDX_CRUISE_ROLLOFF  ("cruise_rolloff_input": "on" / "off" / "N/A")

    // Legacy compatibility aliases synchronized during parsing
    float idle_speed_required;
    float cylinder_head_temp;
    float cylinder_head_temp_voltage;
    float barometric_pressure;
    float map;
    float tps_voltage;
    float intake_air_temp;
    float iat_voltage;
    float idle_air_control;
    float afr_desired;
    float afr_correction_warmup;
    float injector_pw_front;
    float injector_pw_rear;
    float knock_front_retard;
    float knock_rear_retard;
    float accel_enrichment;
    float decel_enleanment;
    float ve_front;
    float ve_rear;
    float engine_run_time;
    char  fuel_mixture_state[16];
    float o2_sensor_front;
    float o2_sensor_rear;
    float afr_front_integrator;
    float afr_rear_integrator;
    char  engine_run_stop_switch[16];
    char  vehicle_theft_deterrent[16];
    char  vehicle_tipped[16];
    char  neutral_switch[16];
    char  clutch_switch[16];
};

/**
 * @brief Extracted telemetry structure for HD ABS (Anti-lock Braking System)
 *        Module (CAN ID 0x7E1 -> Response 0x7E9) DID 0x0200 ("ABS LIVE SENSOR INPUTS", Bytes 0..11).
 */
struct AbsInputs0200 {
    bool     valid;
    uint16_t raw_len;
    uint32_t timestamp_ms;
    uint8_t  family;                    // AbsFamily used to decode this frame (A/B/C/D, see config.h 5B)
    uint16_t value_present;             // bit i set => values[i] (AbsValueIdx) was decoded
    uint32_t state_present;             // bit i set => state i (AbsStateIdx) was decoded
    uint32_t state_on;                  // bit i set => state i asserted ((byte & mask) == mask)
    float    values[ABSV_COUNT];        // decoded engineering values, indexed by AbsValueIdx

    // Convenience copies used by the actuator safety interlocks (0 when not present in this family)
    float    front_wheel_speed;         // km/h
    float    rear_wheel_speed;          // km/h (rear-left on the 4-channel family D)
    float    rear_right_wheel_speed;    // km/h (family D only)
    float    vehicle_speed;             // km/h (family A only)
    float    abs_ecu_module_voltage;    // V (battery voltage, families B/C/D)
};

/**
 * @brief Extracted telemetry structure for HD BCM (Body Control Module)
 *        Module (CAN ID 0x7E2 -> Response 0x7EA) DID 0x0200 ("BCM LIVE BODY & SECURITY INPUTS", 12 Parameters).
 */
struct BcmInputs0200 {
    bool     valid;
    uint16_t raw_len;
    uint32_t timestamp_ms;
    uint32_t update_ms;
    uint8_t  family;            // BcmFamily used to decode this frame (A..E, see config.h 5C)
    uint8_t  raw_bytes[64];

    bool     voltage_present;
    float    system_voltage;    // V (battery voltage as seen by the BCM)
    uint64_t state_present;     // bit i set => state i (BcmStateIdx) was decoded
    uint64_t state_on;          // bit i set => (byte & mask) == mask
};

/**
 * @brief Generic DID 0x0200 telemetry for modules without a dedicated parser
 *        (SCM, LHC, RHC, EHCU, SPEEDO). Layout comes from config.h section 5D.
 */
struct GenInputs0200 {
    bool     valid;
    uint32_t update_ms;
    uint16_t module_id;
    uint8_t  layout;
    uint8_t  count;
    uint8_t  key[GEN_MAX_ITEMS];   // index into GEN_KEYS
    uint8_t  kind[GEN_MAX_ITEMS];  // GenKind
    float    value[GEN_MAX_ITEMS]; // GEN_VAL: scaled value, GEN_FLAG: 0/1
};

/** @brief Result of a generic single-frame UDS request (UdsHandler::rawTransaction). */
struct UdsRawResult {
    bool    tx_ok;
    bool    got_reply;
    bool    positive;     // response SID == request SID + 0x40
    uint8_t nrc;          // valid when got_reply && !positive (0 if the reply was not a 0x7F frame)
    uint8_t len;          // bytes in data[] (response payload starting at the SID)
    uint8_t data[96];
};

class UdsHandler {
public:
    /** Sends one UDS request (1..60 bytes; >7 bytes uses ISO-TP First/Consecutive frames with the ECU's flow control) and waits for the response. Serialized by the bus lock. */
    static bool rawTransaction(uint32_t targetModuleId, const uint8_t *requestBytes, uint8_t requestLength, UdsRawResult &transactionResult, uint32_t timeoutMs = 500);
    static void identifyGenLayout(uint32_t moduleId, const char* calibrationIdString);
    static bool parseGenInputs0200(uint32_t moduleId, const uint8_t *rawPayloadBytes, size_t payloadLength, GenInputs0200 &output);
    static bool readGenInputs0200Synchronous(uint32_t targetModuleId, GenInputs0200 &output, uint32_t timeoutMs = 650);
    static void identifyEcuFamily(const char* calibrationIdString);
    static EcuFamily getCurrentEcuFamily();
    static void resetEcuFamilyLatch();
    static void identifyAbsFamily(const char* calibrationIdString);
    static AbsFamily getCurrentAbsFamily();
    static void identifyBcmFamily(const char* calibrationIdString);
    static BcmFamily getCurrentBcmFamily();
    static const DynamicParamMap* getActiveParamMap();
    static bool sendDiagnosticRequest(uint32_t targetModuleId, uint8_t serviceId, uint8_t subFunction, uint8_t didLowByte = 0x00, const char *rawHexString = nullptr);
    static void armForManualRequest(uint32_t requestId, uint8_t serviceId);
    static void inspectFrame(const twai_message_t &canMessage);
    static bool decodeLocalUdsFrame(uint32_t canIdentifier, uint8_t dataLengthCode, const uint8_t *frameDataBytes, char *outputSummaryBuffer, size_t maxSummaryLength);
    static bool isLocalUdsResponse(uint32_t canIdentifier);
    static void setTesterPresent(bool enableTesterPresent, uint32_t targetModuleId = 0);
    static bool isTesterPresentEnabled();
    static uint32_t getTesterPresentTarget();
    static void setDiagnosticSequenceActive(bool isActive);
    static bool isDiagnosticSequenceActive();
    static void setAutoFlowControl(bool enableAutoFlowControl);
    static bool isAutoFlowControlEnabled();
    static void loop();
    static UdsPendingTransaction getLatestResult();
    /** Reads the identification string that selects the module's layout/INFO list (Parts Number for BCM/SCM/LHC/RHC, otherwise Calibration ID). */
    static bool readFamilyId(uint32_t targetModuleId, char *outputBuffer, size_t outputBufferLength, uint32_t timeoutMs = 350);
    static bool readDidSynchronous(uint32_t targetModuleId, uint16_t did, char *outputAsciiBuffer, size_t maxOutputLength, uint32_t timeoutMs = 600);
    static bool parseEcmInputs0200(const uint8_t *rawPayloadBytes, size_t payloadLength, EcmInputs0200 &output);
    static bool readEcmInputs0200Synchronous(uint32_t targetModuleId, EcmInputs0200 &output, uint32_t timeoutMs = 650);
    static bool readBatteryVoltage0200(uint32_t targetModuleId, float &outputVoltage, float &outputRpm, uint32_t timeoutMs = 350);
    static EcmInputs0200 getLatestEcmInputs0200();
    static bool parseAbsInputs0200(const uint8_t *rawPayloadBytes, size_t payloadLength, AbsInputs0200 &output);
    static bool readAbsInputs0200Synchronous(uint32_t targetModuleId, AbsInputs0200 &output, uint32_t timeoutMs = 650);
    static AbsInputs0200 getLatestAbsInputs0200();
    static bool parseBcmInputs0200(const uint8_t *rawPayloadBytes, size_t payloadLength, BcmInputs0200 &output);
    static bool readBcmInputs0200Synchronous(uint32_t targetModuleId, BcmInputs0200 &output, uint32_t timeoutMs = 650);
    static BcmInputs0200 getLatestBcmInputs0200();
    static bool enterExtendedDiagnosticSession(uint32_t targetModuleId = 0x7E1, uint32_t timeoutMs = 400);
    static void setAbsOutputsPageActive(bool isActive);
    static bool isAbsOutputsPageActive();
    static bool controlActuatorOutput(uint32_t targetModuleId, uint16_t did, uint8_t desiredState, ActuatorOutputResult &outputResult, uint32_t timeoutMs = 550);
    static bool controlAbsActuatorOutput(uint16_t did, uint8_t desiredState, ActuatorOutputResult &outputResult, uint32_t timeoutMs = 550);
    static bool serviceActuatorTimers(uint16_t &outputResetDid, uint32_t &outputTargetModuleId, char *outputTxHexBuffer, size_t maxTxHexLength);
    static void cleanupAllActuatorOutputs(uint32_t targetModuleId = 0x7E0);
    static void cleanupAbsActuatorOutputs();
    static bool isAnyActuatorActive();
    static bool searchModulesSynchronous(uint32_t *outputModuleIds, size_t maxModuleCount, uint8_t *outputModuleCount, uint32_t timeoutMs = 2000);
    static bool readDtcSynchronous(uint32_t targetModuleId, HDDtcRecord *outputDtcRecords, size_t maxDtcCount, uint8_t *outputDtcCount, uint32_t timeoutMs = 1200);
    static bool clearDtcSynchronous(uint32_t targetModuleId, uint32_t timeoutMs = 1000);
    static void cleanAsciiString(const uint8_t *rawBytes, size_t rawByteLength, char *outputString, size_t maxOutputStringLength);
    static void formatDtcCode(uint8_t dtcByte0, uint8_t dtcByte1, uint8_t dtcByte2, uint8_t statusByte, HDDtcRecord &outputRecord);
};
