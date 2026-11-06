/**
 * ============================================================================
 * File      : src/core/config.h
 * Project   : ESP32 HD - UDS Diagnostic Service Tester (ISO 14229 / ISO 15765-2)
 * Copyright : (c) 2026 Henzulea Daniel email: <zulea1@gmail.com>
 * License   : GNU General Public License v3.0 or later (GPL-3.0-or-later) - see LICENSE
 * Version   : see FIRMWARE_VERSION_STR / FIRMWARE_BUILD_NUM_STR in src/core/config.h
 * ============================================================================
 * ARCHITECTURAL & HARDWARE CONFIGURATION OVERVIEW:
 * Dedicated diagnostic service tester for motorcycles equipped
 * with a 500 kbps High-Speed CAN bus (ISO 11898-2).
 *
 * 1. PHYSICAL LAYER (ESP32 TWAI + TJA1043):
 *    - Utilizes the ESP32 internal SJA1000-compatible TWAI controller mapped
 *      to GPIO 22 (TXD) and GPIO 21 (RXD).
 *    - Manages TJA1043 transceiver power states via GPIO 19 (STB_N) and
 *      GPIO 18 (EN), plus hardware fault sensing on GPIO 33 (ERR_N).
 *
 * 2. SAFE BOOT HARDWARE OVERRIDE (GPIO 4):
 *    - GPIO 4 is dedicated as the hardware Safe Boot trigger input.
 *    - Note: GPIO 4 is a general-purpose pin with an internal pull-up; the
 *      firmware enables INPUT_PULLUP, so no external resistor is needed - just
 *      wire a button/jumper from GPIO 4 to GND. It idles HIGH, reads LOW when grounded.
 *    - If GPIO 4 is held LOW when powering up (or pulled LOW for 3 seconds
 *      during runtime), the firmware bypasses normal CAN/UDS initialization
 *      and launches the Safe Boot Rescue Portal for emergency OTA / NVS reset.
 *
 * 3. UDS (ISO 14229) & ISO-TP (ISO 15765-2) DIAGNOSTIC PROTOCOL:
 *    - Physical Request IDs : SCM 0x7D0 (reply 0x7D8), 0x7E0..0x7E3, 0x7E5, 0x7E7, 0x7F1, 0x7F2 (0x7E8..0x7EF are
 *      response IDs for 0x7E0..0x7E7; Response ID = Request ID + 8)
 *    - Broadcast Request ID : 0x7DF (Responses = 0x7E8..0x7EF, 0x7F8..0x7FA)
 *    - Supported Services   :
 *        * 0x10 : DiagnosticSessionControl (0x01 Default, 0x03 Extended)
 *        * 0x3E : TesterPresent (2000 ms background keepalive & 0x7DF scan;
 *                 02 3E 80 00 00 00 00 00 during ABS Output Actuator Tests)
 *        * 0x22 : ReadDataByIdentifier (VIN 0xF1EF, Parts 0xF1EA, Calib 0xF1ED /
 *                 0xF193, Homol 0xF1EE, Dynamic Multi-Profile ECM Live Inputs
 *                 0x0200 [41 parameters across M8 / TC_ETC / TC_CABLE / XL],
 *                 ABS Live Sensor Inputs 0x0200 [13 parameters],
 *                 BCM Live Body & Security Inputs 0x0200 [12 parameters])
 *        * 0x2F : InputOutputControlByIdentifier (ECM Output Actuator Tests:
 *                 0x0501..0x0506, 0x0530; ABS Pump Motor Test: 0x0540 [1s])
 *        * 0x19 : ReadDTCInformation (Sub-function 0x02, Mask 0x09)
 *        * 0x14 : ClearDiagnosticInformation (Group 0xFFFFFF)
 *
 * 4. 4MB FLASH MEMORY & PARTITION LAYOUT (partitions_4mb.csv):
 *    - Total Flash: 4 MB (4,194,304 bytes). SPIFFS removed (Web UI in PROGMEM).
 *    - Dual 1.94 MB (1,984 KB = 2,031,616 bytes) OTA slots (app0 & app1).
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include <string.h>
#include "async_log.h"   // redirects Serial.* to the non-blocking logger task

// ============================================================================
// 1. FIRMWARE VERSION, BUILD & AUTHOR IDENTIFIERS
// ============================================================================
#define APP_NAME_STR             "ESP32 HD"
#define APP_COPYRIGHT_STR        "(c) 2026 Henzulea Daniel email: <zulea1@gmail.com>"
#define FIRMWARE_VERSION_STR     "12"
#define FIRMWARE_BUILD_NUM       12    // numeric build (kept for any ordering/compare); display label is FIRMWARE_BUILD_NUM_STR below
#define _CFG_STRINGIFY2(x)       #x
#define _CFG_STRINGIFY(x)        _CFG_STRINGIFY2(x)
#define FIRMWARE_BUILD_NUM_STR   "12"   // human-facing build label for this release (v12 build 12); build_web_ui.py reads this string for @@FW_BUILD@@
#define OTA_PARTITION_SIZE_BYTES 2883584U // 0x2C0000 (2816 KB) - the single main-app slot (ota_0); factory/safe boot is separate

// Safe Boot (factory partition) firmware carries its OWN version/build, independent of the main app above.
// Increment SAFEBOOT_BUILD_NUM_STR ONLY when the safe-boot firmware itself changes (src/core/safe_boot.cpp,
// src/safeboot_app/, web_src/safeboot.html) - NOT when the main app changes.
#define SAFEBOOT_VERSION_STR     "10"
#define SAFEBOOT_BUILD_NUM_STR   "01"

// ============================================================================
// 2. HARDWARE PINOUT DEFINITIONS (TJA1043 TRANSCEIVER & GPIO 4)
//
//    These are the factory-default / power-on pin assignments. Every one of them
//    (except SAFEBOOT_BUTTON_PIN and STATUS_LED_PIN, which are fixed hardware
//    inputs/outputs unrelated to the CAN transceiver) is also stored, and can be
//    overridden, in NVS via TesterSettings (see section 9 below) - these macros
//    are only the values restored on first boot and after a factory reset.
// ============================================================================
#define CAN_TX_PIN             GPIO_NUM_22 // ESP32 TWAI TX -> Transceiver TXD
#define CAN_RX_PIN             GPIO_NUM_21 // ESP32 TWAI RX <- Transceiver RXD
#define TJA_STB_PIN            GPIO_NUM_19 // TJA1043 STB_N (Active HIGH for Normal Mode)
#define TJA_EN_PIN             GPIO_NUM_18 // TJA1043 EN    (Active HIGH for Normal Mode)
#define TJA_ERR_PIN            GPIO_NUM_33 // TJA1043 ERR_N (Active LOW Fault Output)
#define CAN_ACTIVITY_LED_PIN   GPIO_NUM_2  // Visual CAN bus activity indicator LED
#define SAFEBOOT_BUTTON_PIN    GPIO_NUM_4  // Force SafeBoot input pin (INPUT_PULLUP, Active LOW at power-up - tie to GND)
#define STATUS_LED_PIN         GPIO_NUM_15 // System status LED (Solid on active diagnostic session)

// Default TJA1043 Bus Inactivity Standby Timeout (0s = Disabled / Never Sleep)
#define DEFAULT_TJA_SLEEP_ENABLE      false
#define DEFAULT_TJA_SLEEP_TIMEOUT_SEC 0

// CAN transceiver chip interface selection: TJA1043 (adds Standby/Enable control and
// a hardware fault pin, both driven by CanDriver) vs a plain/generic high-speed CAN transceiver
// (e.g. SN65HVD230, MCP2551 - wired with only TX/RX to the ESP32 TWAI controller, no STB/EN/ERR
// control lines at all). Both share the same CAN_TX_PIN/CAN_RX_PIN by default.
enum CanChipType {
    CAN_CHIP_TJA1043 = 0, // TJA1043: STB/EN standby control + ERR_N fault sensing
    CAN_CHIP_PLAIN   = 1  // Generic transceiver: TX/RX only, no standby control, no fault pin
};

// Factory-default CAN chip type written whenever NVS settings are (re)initialized from scratch -
// a brand-new flash, or after a Safe Boot full factory reset. An already-provisioned device that
// is only upgrading firmware keeps whatever it already had (see main.cpp loadSettings()), so this
// value only governs the "freshly reset" case - the user asked that a full factory reset bring the
// CAN interface back to the simplest possible hardware assumption (plain RX/TX), not TJA1043.
#define DEFAULT_CAN_CHIP_TYPE CAN_CHIP_PLAIN

// ============================================================================
// 3. WI-FI ACCESS POINT & STATION CONFIGURATION
// ============================================================================
// Default mode is STA mode on SSID "ESP32-HD" with password "1122334455".
// If cannot find/connect to STA SSID, automatically enters AP mode with
// SSID "ESP-32-HD" and NO password.
#define DEFAULT_WIFI_MODE_AP   false
#define DEFAULT_WIFI_SSID      "ESP32-HD"
#define DEFAULT_WIFI_PASS      "1122334455"
#define DEFAULT_AP_SSID        "ESP-32-HD"
#define DEFAULT_AP_PASS        ""
#define SAFEBOOT_AP_SSID       "ESP-32-HD-SAFEBOOT"
#define SAFEBOOT_AP_PASS       "1122334455"          // safe-boot rescue AP password (>=8 chars for WPA2)
// Safe boot STA fallback: tried if the NVS-stored Wi-Fi credentials fail to connect, before dropping to the AP.
#define SAFEBOOT_STA_FALLBACK_SSID "ESP32-HD"
#define SAFEBOOT_STA_FALLBACK_PASS "1122334455"

// Default high-speed CAN bitrate
#define DEFAULT_CAN_BITRATE    500000 // 500 kbps (ISO 11898-2)

// ============================================================================
// 4. HD TARGET ECU IDENTIFIERS (UDS ISO 14229)
// ============================================================================
#define HD_ECU_ECM         0x7E0 // Engine Control Module
#define HD_ECU_ABS         0x7E1 // Anti-lock Braking System
#define HD_ECU_BCM         0x7E2 // Body Control Module
#define HD_ECU_SCM         0x7D0 // Security Control Module (reply 0x7D8)
#define HD_ECU_LHC         0x7E3 // Left Hand Control
#define HD_ECU_RHC         0x7E5 // Right Hand Control
#define HD_ECU_EHCU        0x7E7 // Electro-Hydraulic Control Unit (ABS on some models)
#define HD_ECU_RADIO       0x7F1 // Radio / Infotainment GTS
#define HD_ECU_SPEEDO      0x7F2 // Speedometer / Instrument Cluster

#define HD_MIN_DIAG_ID     0x7D0
#define HD_MAX_DIAG_ID     0x7F2
// Module reply IDs: SCM replies on 0x7D8, every other module on request+8 (0x7E8..0x7FF)
#define IS_HD_RX_ID(id)    ((id) == 0x7D8 || ((id) >= 0x7E8 && (id) <= 0x7FF))

// Standard UDS Service Identifiers
#define UDS_SID_DIAG_SESSION_CTRL   0x10
#define UDS_SID_ECU_RESET           0x11
#define UDS_SID_CLEAR_DTC           0x14
#define UDS_SID_READ_DTC            0x19
#define UDS_SID_READ_DID            0x22
#define UDS_SID_WRITE_DID           0x2E
#define UDS_SID_IO_CTRL_BY_ID       0x2F
#define UDS_SID_ROUTINE_CTRL        0x31
#define UDS_SID_TESTER_PRESENT      0x3E

// HD ECU Information & Live Telemetry Data Identifiers (DIDs)
#define HD_DID_ECM_LIVE_INPUTS  0x0200 // ECM Live Module Inputs Telemetry Block (41 Dynamic Parameters)
#define HD_DID_ABS_LIVE_INPUTS  0x0200 // ABS Live Sensor Inputs Telemetry Block (12 Bytes, 13 Parameters)
#define HD_DID_BCM_LIVE_INPUTS  0x0200 // BCM Live Body & Security Inputs Telemetry Block (8+ Bytes, 12 Parameters)
#define HD_DID_VIN              0xF1EF // Vehicle Identification Number
#define HD_DID_PARTS_NUMBER     0xF1EA // ECU Hardware / Assembly Parts Number
#define HD_DID_CALIBRATION_ID   0xF1ED // Calibration Software ID (Primary)
#define HD_DID_CALIBRATION_F193 0xF193 // Calibration Software ID (ISO 14229 F193)
#define HD_DID_HOMOLOGATION_ID  0xF1EE // Homologation / Regulatory ID

// HD ECM (0x7E0) & ABS (0x7E1) Output Actuator Control Data Identifiers (DIDs) for Service 0x2F
#define HD_DID_ACT_COOLING_FAN      0x0501 // ECM Cooling Fan Relay        (Toggle ON/OFF)
#define HD_DID_ACT_FUEL_PUMP        0x0502 // ECM Fuel Pump Relay          (Pulse 2000ms -> Auto Reset)
#define HD_DID_ACT_FRONT_INJECTOR   0x0503 // ECM Front Injector Actuator  (Test Output -> Auto Reset)
#define HD_DID_ACT_REAR_INJECTOR    0x0504 // ECM Rear Injector Actuator   (Test Output -> Auto Reset)
#define HD_DID_ACT_FRONT_COIL       0x0505 // ECM Front Ignition Coil      (Test Output -> Auto Reset)
#define HD_DID_ACT_REAR_COIL        0x0506 // ECM Rear Ignition Coil       (Test Output -> Auto Reset)
#define HD_DID_ACT_CHECK_ENGINE_MIL 0x0530 // ECM Check Engine Light / MIL (Toggle ON/OFF)
#define HD_DID_ACT_ABS_PUMP_MOTOR   0x0540 // ABS Hydraulic Pump Motor     (Pulse 1000ms -> Auto Reset on 0x7E1)

// UDS 0x2F InputOutputControlParameter values
#define UDS_IO_PARAM_RETURN_TO_ECU      0x00   // ReturnControlToECU
#define UDS_IO_PARAM_SHORT_TERM_ADJ     0x03   // ShortTermAdjustment

// Battery Voltage Safeguard Threshold during Active Actuator Tests (Byte 0 of DID 0x0200)
#define BATTERY_LOW_WARN_VOLTS          11.8f

// Non-blocking Asynchronous Pulse Durations (millis())
#define ACTUATOR_FUEL_PUMP_PULSE_MS     2000U  // 2000 ms asynchronous pulse for ECM Fuel Pump Relay (0x0502)
#define ACTUATOR_ABS_PUMP_PULSE_MS      1000U  // 1000 ms asynchronous pulse for ABS Hydraulic Pump Motor (0x0540)
#define ACTUATOR_MOMENTARY_PULSE_MS     800U   // 800 ms asynchronous pulse for Coil/Injector Test Output

// ============================================================================
// 5. DYNAMIC MULTI-GENERATION ECU FAMILY MATRICES FOR DID 0x0200
//    (Milwaukee-Eight, Twin Cam ETC, Twin Cam Cable, Sportster / Street XL)
// ============================================================================
enum EcuFamily {
    ECU_FAMILY_UNKNOWN = 0,
    ECU_FAMILY_M8,        // Table_MAP_M8_0200
    ECU_FAMILY_TC_ETC,    // Table_MAP_TC_ETC_0200
    ECU_FAMILY_TC_CABLE,  // Table_MAP_TC_CABLE_0200
    ECU_FAMILY_XL         // Table_MAP_XL_0200
};

struct DynamicParamMap {
    uint8_t byte_offset;
    uint8_t data_length;  // 1 for uint8_t, 2 for uint16_t Big-Endian
    uint8_t bit_mask;     // 0xFF for pure analog variables; specific bitmask for flags
    float   scaling;
    float   offset;
    const char* json_key; // Directly bind properties to WebSocket JSON keys
};

enum ParamIndex {
    IDX_BATT_VOLT = 0,    IDX_MAP_KPA,          IDX_MAP_VOLTS,        IDX_ENGINE_TEMP,
    IDX_ENGINE_TEMP_V,    IDX_IAT_DEG,          IDX_IAT_VOLTS,        IDX_THROTTLE_PCT,
    IDX_VALID_TPS_V1,     IDX_VALID_TPS_V2,     IDX_TWIST_PCT,        IDX_TWIST_V1,
    IDX_TWIST_V2,         IDX_IDLE_SET_RPM,     IDX_RPM,              IDX_SPEED,
    IDX_FL_INT,           IDX_RL_INT,           IDX_FL_AFV,           IDX_RL_AFV,
    IDX_O2_FRONT,         IDX_O2_REAR,          IDX_INJ_F,            IDX_INJ_R,
    IDX_SPARK_F,          IDX_SPARK_R,          IDX_KNOCK_F,          IDX_KNOCK_R,
    IDX_IAC_STEPS,        IDX_FLAG_RUN,         IDX_FLAG_SECURITY,    IDX_FLAG_BANK_ANGLE,
    IDX_FLAG_NEUTRAL,     IDX_FLAG_CLUTCH,      IDX_GEAR_POSITION,    IDX_CRUISE_BRAKE,
    IDX_CRUISE_SET,       IDX_CRUISE_RES,       IDX_CRUISE_ENABLE,    IDX_CRUISE_MODE,
    IDX_CRUISE_ROLLOFF,   TOTAL_PARAM_COUNT
};

// ----------------------------------------------------------------------------
//    Offsets/scalings/masks below are for command 220200. Byte offsets are 0-based
//    AFTER the 62 02 00 header (byte 0 = battery voltage). 2-byte values are Big-Endian.
// 5a. CALIBRATION-ID PREFIX -> FAMILY RULES  (first 2 / 3 / 5 characters of the ID)
// ----------------------------------------------------------------------------
//  Family          | Prefix rule                              | Map table             | Database set
//  ----------------+------------------------------------------+-----------------------+------------
//  ECU_FAMILY_M8   | 67xxx / 64xxx / 32107-xxxx               | Map_M8_0200           | 8FB0EC
//  ECU_FAMILY_TC_ETC | 410xxxxx / 325xxx / 324xxx / 32109-xxxx / DEFAULT | Map_TC_ETC_0200       | 8F717C
//  ECU_FAMILY_TC_CABLE | numeric prefix < 300 (fallback)          | Map_TC_CABLE_0200     | 8F637C
//  ECU_FAMILY_XL   | 329xxx or ID containing 'XL'             | Map_XL_0200           | 8F81AC
//  (default)       | anything that matches no rule            | Map_TC_ETC_0200       | 8F717C
//  Check order: 'XL' substring -> 5-digit -> 3-digit -> 2-digit -> numeric < 300 -> default.

struct EcuPrefixRule {
    const char* prefix;   // compared with strncmp() against the trimmed calibration ID
    EcuFamily   family;
};

static const EcuPrefixRule ECU_PREFIX_RULES[] = {
    // 5-digit prefixes (checked first)
    {"32107", ECU_FAMILY_M8},
    {"32109", ECU_FAMILY_TC_ETC},
    // 3-digit prefixes
    {"410",   ECU_FAMILY_TC_ETC},
    {"325",   ECU_FAMILY_TC_ETC},
    {"324",   ECU_FAMILY_TC_ETC},
    {"329",   ECU_FAMILY_XL},
    // 2-digit prefixes
    {"67",    ECU_FAMILY_M8},
    {"64",    ECU_FAMILY_M8}
};
static const size_t ECU_PREFIX_RULE_COUNT = sizeof(ECU_PREFIX_RULES) / sizeof(ECU_PREFIX_RULES[0]);

// DEFAULT family when the calibration ID matches nothing (or could not be read).
#define ECU_FAMILY_DEFAULT        ECU_FAMILY_TC_ETC
// Legacy cable-throttle fallback: numeric 3-digit prefix below this value.
#define ECU_CABLE_NUMERIC_LIMIT   300

// Classify a calibration ID string (e.g. "41000188F") into an EcuFamily.
// Pure function, no side effects. 'out_rule' (optional) receives the rule that fired.
static inline EcuFamily classifyCalibrationId(const char* cal, const char** out_rule = nullptr) {
    if (out_rule) *out_rule = "default";
    if (cal == nullptr) return ECU_FAMILY_DEFAULT;
    while (*cal == ' ' || *cal == '\t') cal++;                       // trim leading blanks
    if (*cal == '\0') return ECU_FAMILY_DEFAULT;

    // 1. Identifier explicitly containing "XL" (case-insensitive) -> Sportster / Street
    for (const char* p = cal; p[0] && p[1]; p++) {
        if ((p[0] == 'X' || p[0] == 'x') && (p[1] == 'L' || p[1] == 'l')) {
            if (out_rule) *out_rule = "contains XL";
            return ECU_FAMILY_XL;
        }
    }
    // 2. Prefix rules (5-digit, 3-digit, 2-digit - table is already in that order)
    for (size_t i = 0; i < ECU_PREFIX_RULE_COUNT; i++) {
        if (strncmp(cal, ECU_PREFIX_RULES[i].prefix, strlen(ECU_PREFIX_RULES[i].prefix)) == 0) {
            if (out_rule) *out_rule = ECU_PREFIX_RULES[i].prefix;
            return ECU_PREFIX_RULES[i].family;
        }
    }
    // 3. Legacy numeric prefix below the 300 series -> Twin Cam cable throttle
    if (cal[0] >= '0' && cal[0] <= '9' && cal[1] >= '0' && cal[1] <= '9' && cal[2] >= '0' && cal[2] <= '9') {
        int n = (cal[0] - '0') * 100 + (cal[1] - '0') * 10 + (cal[2] - '0');
        if (n < ECU_CABLE_NUMERIC_LIMIT) {
            if (out_rule) *out_rule = "numeric < 300";
            return ECU_FAMILY_TC_CABLE;
        }
    }
    return ECU_FAMILY_DEFAULT;
}

// ----------------------------------------------------------------------------
// 5b. PER-FAMILY MAP TABLES  (value = raw * scaling + offset)
// ----------------------------------------------------------------------------

// 1. Table_MAP_M8_0200 - Milwaukee-Eight 2017+ (UDS, dual MAP)
//    Calibration prefixes : 67xxx / 64xxx / 32107-xxxx
//    Database expression set: 8FB0EC  (keys 8FB0EC000001..)  | equivalent sets: A4113C, A41DB4, A429BC, 904664
const DynamicParamMap Map_M8_0200[TOTAL_PARAM_COUNT] = {
    {0, 1, 0xFF, 0.1f, 0.0f, "battery_voltage"},
    {1, 2, 0xFF, 0.03125f, 0.0f, "map_kpa"},
    {17, 1, 0xFF, 0.019531f, 0.0f, "map_volts"},
    {8, 1, 0xFF, 1.0f, -16.0f, "engine_temperature"},
    {13, 1, 0xFF, 0.019531f, 0.0f, "engine_temperature_sensor"},
    {11, 1, 0xFF, 1.0f, -40.0f, "intake_air_temperature"},
    {14, 1, 0xFF, 0.019531f, 0.0f, "intake_air_temperature_sensor"},
    {5, 1, 0xFF, 0.4545f, 0.0f, "throttle_position"},
    {19, 1, 0xFF, 0.019531f, 0.0f, "throttle_position_sensor_1"},
    {20, 1, 0xFF, 0.019531f, 0.0f, "throttle_position_sensor_2"},
    {6, 1, 0xFF, 0.3922f, 0.0f, "twistgrip_position"},
    {21, 1, 0xFF, 0.019531f, 0.0f, "twistgrip_sensor_1"},
    {22, 1, 0xFF, 0.019531f, 0.0f, "twistgrip_sensor_2"},
    {23, 1, 0xFF, 8.0f, 0.0f, "idle_set_speed"},
    {24, 2, 0xFF, 1.0f, 0.0f, "engine_speed"},
    {60, 1, 0xFF, 1.0f, 0.0f, "vehicle_speed"},
    {54, 1, 0xFF, 0.78125f, 0.0f, "front_closed_loop_integrator"},
    {55, 1, 0xFF, 0.78125f, 0.0f, "rear_closed_loop_integrator"},
    {56, 1, 0xFF, 0.78125f, 0.0f, "front_adaptive_fuel_value"},
    {57, 1, 0xFF, 0.78125f, 0.0f, "rear_adaptive_fuel_value"},
    {52, 1, 0xFF, 0.02f, 0.0f, "front_oxygen_sensor"},
    {53, 1, 0xFF, 0.02f, 0.0f, "rear_oxygen_sensor"},
    {30, 2, 0xFF, 0.004f, 0.0f, "injector_time_front"},
    {32, 2, 0xFF, 0.004f, 0.0f, "injector_time_rear"},
    {29, 1, 0xFF, 0.5f, 0.0f, "spark_advance_front"},
    {28, 1, 0xFF, 0.5f, 0.0f, "spark_advance_rear"},
    {42, 1, 0xFF, 0.25f, 0.0f, "front_spark_knock_retard"},
    {43, 1, 0xFF, 0.25f, 0.0f, "rear_spark_knock_retard"},
    {71, 1, 0xFF, 1.0f, 0.0f, "idle_air_control_steps"},
    {72, 1, 0x01, 1.0f, 0.0f, "run_mode_flag"},
    {0,  0, 0x00, 0.00000f, 0.0f,   "tssm_security_state"},  // N/A in this family
    {71, 1, 0x08, 1.0f, 0.0f, "bank_angle_sensor_mode"},
    {72, 1, 0x10, 1.0f, 0.0f, "neutral_switch_input"},
    {72, 1, 0x08, 1.0f, 0.0f, "clutch_lever_switch_state"},
    {0,  0, 0x00, 0.00000f, 0.0f,   "gear_position"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_brake_input"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_set"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_res"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_enable_switch"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_mode"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_rolloff_input"}  // N/A in this family
};

// 2. Table_MAP_TC_ETC_0200 - Twin Cam Electronic Throttle 2008-2016 (DEFAULT)
//    Calibration prefixes : 410xxxxx / 325xxx / 324xxx / 32109-xxxx / DEFAULT
//    Database expression set: 8F717C  (keys 8F717C000001..)  | equivalent sets: 8F526C, 8F5AF4, 8FBEEC, 8F98DC, 8F8804, 8FA7BC
const DynamicParamMap Map_TC_ETC_0200[TOTAL_PARAM_COUNT] = {
    {0, 1, 0xFF, 0.1f, 0.0f, "battery_voltage"},
    {1, 1, 0xFF, 0.369f, 10.3f, "map_kpa"},
    {15, 1, 0xFF, 0.019531f, 0.0f, "map_volts"},
    {6, 1, 0xFF, 1.0f, -16.0f, "engine_temperature"},
    {11, 1, 0xFF, 0.019531f, 0.0f, "engine_temperature_sensor"},
    {9, 1, 0xFF, 1.0f, -16.0f, "intake_air_temperature"},
    {12, 1, 0xFF, 0.019531f, 0.0f, "intake_air_temperature_sensor"},
    {3, 1, 0xFF, 0.4545f, 0.0f, "throttle_position"},
    {17, 1, 0xFF, 0.019531f, 0.0f, "throttle_position_sensor_1"},
    {18, 1, 0xFF, 0.019531f, 0.0f, "throttle_position_sensor_2"},
    {4, 1, 0xFF, 0.3922f, 0.0f, "twistgrip_position"},
    {19, 1, 0xFF, 0.019531f, 0.0f, "twistgrip_sensor_1"},
    {20, 1, 0xFF, 0.019531f, 0.0f, "twistgrip_sensor_2"},
    {21, 1, 0xFF, 8.0f, 0.0f, "idle_set_speed"},
    {22, 2, 0xFF, 1.0f, 0.0f, "engine_speed"},
    {58, 1, 0xFF, 1.0f, 0.0f, "vehicle_speed"},
    {52, 1, 0xFF, 0.78125f, 0.0f, "front_closed_loop_integrator"},
    {53, 1, 0xFF, 0.78125f, 0.0f, "rear_closed_loop_integrator"},
    {54, 1, 0xFF, 0.78125f, 0.0f, "front_adaptive_fuel_value"},
    {55, 1, 0xFF, 0.78125f, 0.0f, "rear_adaptive_fuel_value"},
    {50, 1, 0xFF, 0.02f, 0.0f, "front_oxygen_sensor"},
    {51, 1, 0xFF, 0.02f, 0.0f, "rear_oxygen_sensor"},
    {28, 2, 0xFF, 0.004f, 0.0f, "injector_time_front"},
    {30, 2, 0xFF, 0.004f, 0.0f, "injector_time_rear"},
    {27, 1, 0xFF, 0.5f, 0.0f, "spark_advance_front"},
    {26, 1, 0xFF, 0.5f, 0.0f, "spark_advance_rear"},
    {40, 1, 0xFF, 0.25f, 0.0f, "front_spark_knock_retard"},
    {41, 1, 0xFF, 0.25f, 0.0f, "rear_spark_knock_retard"},
    {69, 1, 0xFF, 1.0f, 0.0f, "idle_air_control_steps"},
    {70, 1, 0x01, 1.0f, 0.0f, "run_mode_flag"},
    {27, 1, 0x80, 1.0f, 0.0f, "tssm_security_state"},
    {71, 1, 0x08, 1.0f, 0.0f, "bank_angle_sensor_mode"},
    {70, 1, 0x10, 1.0f, 0.0f, "neutral_switch_input"},
    {70, 1, 0x08, 1.0f, 0.0f, "clutch_lever_switch_state"},
    {59, 1, 0xFF, 1.0f, 0.0f, "gear_position"},
    {56, 1, 0x01, 1.0f, 0.0f, "cruise_brake_input"},
    {56, 1, 0x02, 1.0f, 0.0f, "cruise_set"},
    {56, 1, 0x04, 1.0f, 0.0f, "cruise_res"},
    {56, 1, 0x10, 1.0f, 0.0f, "cruise_enable_switch"},
    {56, 1, 0x20, 1.0f, 0.0f, "cruise_mode"},
    {56, 1, 0x08, 1.0f, 0.0f, "cruise_rolloff_input"}
};

// 3. Table_MAP_TC_CABLE_0200 - Twin Cam Cable Throttle 2001-2009
//    Calibration prefixes : numeric prefix < 300 (fallback)
//    Database expression set: 8F637C  (keys 8F637C000001..)  | equivalent sets: 8F6A7C, 8F7AAC, 8F91DC (identical); 8F454C, 8F4BDC (no TSSM / bank angle)
const DynamicParamMap Map_TC_CABLE_0200[TOTAL_PARAM_COUNT] = {
    {0, 1, 0xFF, 0.1f, 0.0f, "battery_voltage"},
    {1, 1, 0xFF, 0.369f, 10.3f, "map_kpa"},
    {15, 1, 0xFF, 0.019531f, 0.0f, "map_volts"},
    {6, 1, 0xFF, 1.0f, -16.0f, "engine_temperature"},
    {11, 1, 0xFF, 0.019531f, 0.0f, "engine_temperature_sensor"},
    {9, 1, 0xFF, 1.0f, -16.0f, "intake_air_temperature"},
    {12, 1, 0xFF, 0.019531f, 0.0f, "intake_air_temperature_sensor"},
    {3, 1, 0xFF, 0.4545f, 0.0f, "throttle_position"},
    {16, 1, 0xFF, 0.019531f, 0.0f, "throttle_position_sensor_1"},
    {0,  0, 0xFF, 0.00000f, 0.0f,   "throttle_position_sensor_2"},  // N/A in this family
    {0,  0, 0xFF, 0.00000f, 0.0f,   "twistgrip_position"},  // N/A in this family
    {0,  0, 0xFF, 0.00000f, 0.0f,   "twistgrip_sensor_1"},  // N/A in this family
    {0,  0, 0xFF, 0.00000f, 0.0f,   "twistgrip_sensor_2"},  // N/A in this family
    {21, 1, 0xFF, 8.0f, 0.0f, "idle_set_speed"},
    {22, 2, 0xFF, 1.0f, 0.0f, "engine_speed"},
    {58, 1, 0xFF, 1.0f, 0.0f, "vehicle_speed"},
    {52, 1, 0xFF, 0.78125f, 0.0f, "front_closed_loop_integrator"},
    {53, 1, 0xFF, 0.78125f, 0.0f, "rear_closed_loop_integrator"},
    {54, 1, 0xFF, 0.78125f, 0.0f, "front_adaptive_fuel_value"},
    {55, 1, 0xFF, 0.78125f, 0.0f, "rear_adaptive_fuel_value"},
    {50, 1, 0xFF, 0.02f, 0.0f, "front_oxygen_sensor"},
    {51, 1, 0xFF, 0.02f, 0.0f, "rear_oxygen_sensor"},
    {28, 2, 0xFF, 0.004f, 0.0f, "injector_time_front"},
    {30, 2, 0xFF, 0.004f, 0.0f, "injector_time_rear"},
    {27, 1, 0xFF, 0.5f, 0.0f, "spark_advance_front"},
    {26, 1, 0xFF, 0.5f, 0.0f, "spark_advance_rear"},
    {40, 1, 0xFF, 0.25f, 0.0f, "front_spark_knock_retard"},
    {41, 1, 0xFF, 0.25f, 0.0f, "rear_spark_knock_retard"},
    {69, 1, 0xFF, 1.0f, 0.0f, "idle_air_control_steps"},
    {70, 1, 0x01, 1.0f, 0.0f, "run_mode_flag"},
    {27, 1, 0x80, 1.0f, 0.0f, "tssm_security_state"},
    {71, 1, 0x08, 1.0f, 0.0f, "bank_angle_sensor_mode"},
    {70, 1, 0x10, 1.0f, 0.0f, "neutral_switch_input"},
    {70, 1, 0x08, 1.0f, 0.0f, "clutch_lever_switch_state"},
    {59, 1, 0xFF, 1.0f, 0.0f, "gear_position"},
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_brake_input"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_set"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_res"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_enable_switch"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_mode"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_rolloff_input"}  // N/A in this family
};

// 4. Table_MAP_XL_0200 - Sportster Evolution / V-Rod / Street
//    Calibration prefixes : 329xxx or ID containing 'XL'
//    Database expression set: 8F81AC  (keys 8F81AC000001..)  | equivalent sets: 8FA0F4 (identical)
const DynamicParamMap Map_XL_0200[TOTAL_PARAM_COUNT] = {
    {0, 1, 0xFF, 0.1f, 0.0f, "battery_voltage"},
    {1, 1, 0xFF, 0.369f, 10.3f, "map_kpa"},
    {15, 1, 0xFF, 0.019531f, 0.0f, "map_volts"},
    {6, 1, 0xFF, 1.0f, -16.0f, "engine_temperature"},
    {11, 1, 0xFF, 0.019531f, 0.0f, "engine_temperature_sensor"},
    {9, 1, 0xFF, 1.0f, -16.0f, "intake_air_temperature"},
    {12, 1, 0xFF, 0.019531f, 0.0f, "intake_air_temperature_sensor"},
    {3, 1, 0xFF, 0.4545f, 0.0f, "throttle_position"},
    {16, 1, 0xFF, 0.019531f, 0.0f, "throttle_position_sensor_1"},
    {0,  0, 0xFF, 0.00000f, 0.0f,   "throttle_position_sensor_2"},  // N/A in this family
    {0,  0, 0xFF, 0.00000f, 0.0f,   "twistgrip_position"},  // N/A in this family
    {0,  0, 0xFF, 0.00000f, 0.0f,   "twistgrip_sensor_1"},  // N/A in this family
    {0,  0, 0xFF, 0.00000f, 0.0f,   "twistgrip_sensor_2"},  // N/A in this family
    {21, 1, 0xFF, 8.0f, 0.0f, "idle_set_speed"},
    {22, 2, 0xFF, 1.0f, 0.0f, "engine_speed"},
    {58, 1, 0xFF, 1.0f, 0.0f, "vehicle_speed"},
    {52, 1, 0xFF, 0.78125f, 0.0f, "front_closed_loop_integrator"},
    {53, 1, 0xFF, 0.78125f, 0.0f, "rear_closed_loop_integrator"},
    {54, 1, 0xFF, 0.78125f, 0.0f, "front_adaptive_fuel_value"},
    {55, 1, 0xFF, 0.78125f, 0.0f, "rear_adaptive_fuel_value"},
    {50, 1, 0xFF, 0.02f, 0.0f, "front_oxygen_sensor"},
    {51, 1, 0xFF, 0.02f, 0.0f, "rear_oxygen_sensor"},
    {28, 2, 0xFF, 0.004f, 0.0f, "injector_time_front"},
    {30, 2, 0xFF, 0.004f, 0.0f, "injector_time_rear"},
    {27, 1, 0xFF, 0.5f, 0.0f, "spark_advance_front"},
    {26, 1, 0xFF, 0.5f, 0.0f, "spark_advance_rear"},
    {40, 1, 0xFF, 0.25f, 0.0f, "front_spark_knock_retard"},
    {41, 1, 0xFF, 0.25f, 0.0f, "rear_spark_knock_retard"},
    {69, 1, 0xFF, 1.0f, 0.0f, "idle_air_control_steps"},
    {70, 1, 0x01, 1.0f, 0.0f, "run_mode_flag"},
    {27, 1, 0x80, 1.0f, 0.0f, "tssm_security_state"},
    {71, 1, 0x08, 1.0f, 0.0f, "bank_angle_sensor_mode"},
    {0,  0, 0x00, 0.00000f, 0.0f,   "neutral_switch_input"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "clutch_lever_switch_state"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "gear_position"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_brake_input"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_set"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_res"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_enable_switch"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_mode"},  // N/A in this family
    {0,  0, 0x00, 0.00000f, 0.0f,   "cruise_rolloff_input"}  // N/A in this family
};

// ----------------------------------------------------------------------------
// 5c. OPTIONAL EXTRA M8 PARAMETERS (set 8FB0EC items that have no slot in the 41-slot
//     ParamIndex). Not consumed by parseEcmInputs0200(); add a second parse loop or
//     extend ParamIndex/EcmInputs0200 if you want them on the WebSocket JSON.
// ----------------------------------------------------------------------------
const DynamicParamMap Extra_M8_0200[] = {
    {40, 1, 0xFF, 1.0f, 0.0f, "knock_counter_front"},
    {41, 1, 0xFF, 1.0f, 0.0f, "knock_counter_rear"},
    {99, 2, 0xFF, 0.03125f, 0.0f, "manifold_absolute_pressure_rear"},
    {12, 1, 0xFF, 0.369f, 10.3f, "barometric_pressure"},
    {101, 1, 0xFF, 0.019531f, 0.0f, "manifold_absolute_pressure_sensor_rear"},
    {38, 1, 0xFF, 0.1f, 0.0f, "knock_intensity_front"},
    {39, 1, 0xFF, 0.1f, 0.0f, "knock_intensity_rear"},
    {72, 1, 0x02, 1.00000f, 0.0f, "fuel_disabled"},
    {84, 1, 0xFF, 0.019531f, 0.0f, "knock_f"},
    {85, 1, 0xFF, 0.019531f, 0.0f, "knock_r"},
    {86, 1, 0xFF, 20.0f, 0.0f, "o2_sensor_return"},
    {87, 1, 0xFF, 20.0f, 0.0f, "post_cat_o2_sensor_front"},
    {88, 1, 0xFF, 20.0f, 0.0f, "post_cat_o2_sensor_rear"},
    {91, 1, 0xFF, 0.1f, 0.0f, "fan_select_switch"},
    {92, 1, 0xFF, 0.019531f, 0.0f, "gear_position_sensor"},
    {93, 1, 0xFF, 0.019531f, 0.0f, "shift_sensor"}
};
const size_t EXTRA_M8_0200_COUNT = sizeof(Extra_M8_0200) / sizeof(Extra_M8_0200[0]);

// ----------------------------------------------------------------------------
// 5d. KNOWN SUB-VARIANT DELTAS (other database sets of the same family; same table, but):
//   M8  A4113C : no Engine Temperature slot  | A41DB4 : no Engine Temp / Clutch, Bank Angle @73
//   M8  A429BC : MAP is 16-bit @1 x0.03125 (single MAP), no Engine Temp / Clutch, Bank Angle @73
//   M8  904664 : MAP 16-bit @1 x0.03125, MAP sensor @17, Engine Temp = x-40, IAT = x-16 (swapped vs 8FB0EC)
//   ETC 8F526C/8F5AF4/8FBEEC : no TSSM / Bank Angle / Gear   | 8F98DC/8F8804/8FA7BC : no Cruise inputs
//   CABLE 8F454C/8F4BDC : no TSSM / Bank Angle
// ----------------------------------------------------------------------------

// ============================================================================
// 5B. ABS (0x7E1) DID 0x0200 FAMILY MAPS
//     There are four distinct DID 0x0200 layouts. The family is chosen from the ABS Calibration ID
//     (DID 0xF1ED / 0xF193 read from 0x7E1) by part-number prefix.
//     All offsets are 0-based bytes AFTER the 62 02 00 response header; 16-bit values are
//     big-endian; value = raw * scale + add; states are (byte & mask) == mask.
//     Items whose offset lies beyond the received payload are skipped (never decoded).
// ============================================================================
enum AbsFamily : uint8_t { ABS_FAMILY_A = 0, ABS_FAMILY_B, ABS_FAMILY_C, ABS_FAMILY_D, ABS_FAMILY_COUNT };
#define ABS_FAMILY_DEFAULT        ABS_FAMILY_C   // the default family when no part-number prefix matches

enum AbsValueIdx : uint8_t {
    ABSV_FRONT_WHEEL_SPEED, ABSV_REAR_WHEEL_SPEED, ABSV_REAR_RIGHT_WHEEL_SPEED, ABSV_VEHICLE_SPEED, ABSV_ABS_ECU_MODULE_VOLTAGE, ABSV_FRONT_ACTUATOR_CURRENT, ABSV_REAR_ACTUATOR_CURRENT, ABSV_FRONT_ACTUATOR_VOLTAGE, ABSV_REAR_ACTUATOR_VOLTAGE, ABSV_FRONT_MASTER_CYLINDER_PRESSURE, ABSV_REAR_MASTER_CYLINDER_PRESSURE, ABSV_FRONT_CALIPER_CIRCUIT_PRESSURE, ABSV_REAR_CALIPER_CIRCUIT_PRESSURE, ABSV_COUNT
};
enum AbsStateIdx : uint8_t {
    ABSS_ABS_ENABLED, ABSS_ABS_FAILED, ABSS_ABS_ACTIVE, ABSS_FRONT_WHEEL_SENSOR_FAULT, ABSS_REAR_WHEEL_SENSOR_FAULT, ABSS_REAR_RIGHT_WHEEL_SENSOR_FAULT, ABSS_FRONT_BRAKE_SWITCH, ABSS_REAR_BRAKE_SWITCH, ABSS_PUMP_MOTOR_STATUS, ABSS_FRONT_INLET_STATUS, ABSS_FRONT_OUTLET_STATUS, ABSS_REAR_INLET_STATUS, ABSS_REAR_OUTLET_STATUS, ABSS_REAR_RIGHT_INLET_STATUS, ABSS_REAR_RIGHT_OUTLET_STATUS, ABSS_FRONT_ACTUATOR, ABSS_REAR_ACTUATOR, ABSS_FRONT_RELEASE_SOLENOID, ABSS_FRONT_APPLY_SOLENOID, ABSS_REAR_RELEASE_SOLENOID, ABSS_REAR_APPLY_SOLENOID, ABSS_COUNT
};

static const char* const ABS_VALUE_KEYS[ABSV_COUNT] = {
    "front_wheel_speed", "rear_wheel_speed", "rear_right_wheel_speed", "vehicle_speed", "abs_ecu_module_voltage", "front_actuator_current", "rear_actuator_current", "front_actuator_voltage", "rear_actuator_voltage", "front_master_cylinder_pressure", "rear_master_cylinder_pressure", "front_caliper_circuit_pressure", "rear_caliper_circuit_pressure"
};
static const char* const ABS_STATE_KEYS[ABSS_COUNT] = {
    "abs_enabled", "abs_failed", "abs_active",
    "front_wheel_sensor_fault", "rear_wheel_sensor_fault", "rear_right_wheel_sensor_fault",
    "front_brake_switch", "rear_brake_switch", "pump_motor_status",
    "front_inlet_status", "front_outlet_status", "rear_inlet_status",
    "rear_outlet_status", "rear_right_inlet_status", "rear_right_outlet_status",
    "front_actuator", "rear_actuator", "front_release_solenoid",
    "front_apply_solenoid", "rear_release_solenoid", "rear_apply_solenoid"
};
// 1 = state is reported "Yes"/"No" (faults, ABS flags), 0 = "On"/"Off" (switches, solenoids, pump)
static const uint8_t ABS_STATE_YESNO[ABSS_COUNT] = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

struct AbsValueDef { uint8_t idx; uint8_t offset; uint8_t len; float scale; float add; };
struct AbsStateDef { uint8_t idx; uint8_t offset; uint8_t mask; };
struct AbsFamilyMap {
    const char*        name;
    const AbsValueDef* values;  uint8_t value_count;
    const AbsStateDef* states;  uint8_t state_count;
};

// Family A: 16-bit wheel speeds + actuator current/voltage
//   Database sets : 8FC934 / 8FD45C
//   Calibration  : 40662-11, 41100036   (full payload = 19 bytes after header)
static const AbsValueDef AbsMapA_Values[] = {
    {ABSV_FRONT_WHEEL_SPEED, 0, 2, 0.0078f, 0.0f},
    {ABSV_REAR_WHEEL_SPEED, 2, 2, 0.0078f, 0.0f},
    {ABSV_VEHICLE_SPEED, 5, 2, 0.0156f, 0.0f},
    {ABSV_FRONT_ACTUATOR_CURRENT, 8, 2, 0.0156f, 0.0f},
    {ABSV_REAR_ACTUATOR_CURRENT, 10, 2, 0.0156f, 0.0f},
    {ABSV_FRONT_ACTUATOR_VOLTAGE, 15, 2, 0.000977f, 0.0f},
    {ABSV_REAR_ACTUATOR_VOLTAGE, 17, 2, 0.000977f, 0.0f}
};
static const AbsStateDef AbsMapA_States[] = {
    {ABSS_ABS_ENABLED, 4, 0x08},
    {ABSS_ABS_FAILED, 4, 0x02},
    {ABSS_ABS_ACTIVE, 4, 0x04},
    {ABSS_REAR_WHEEL_SENSOR_FAULT, 4, 0x40},
    {ABSS_FRONT_WHEEL_SENSOR_FAULT, 4, 0x80},
    {ABSS_FRONT_BRAKE_SWITCH, 4, 0x20},
    {ABSS_REAR_BRAKE_SWITCH, 4, 0x10},
    {ABSS_REAR_ACTUATOR, 12, 0x04},
    {ABSS_FRONT_ACTUATOR, 12, 0x08},
    {ABSS_REAR_RELEASE_SOLENOID, 12, 0x10},
    {ABSS_REAR_APPLY_SOLENOID, 12, 0x20},
    {ABSS_FRONT_RELEASE_SOLENOID, 12, 0x40},
    {ABSS_FRONT_APPLY_SOLENOID, 12, 0x80}
};

// Family B: 8-bit basic (wheel speed, battery, pump/valves)
//   Database sets : 8FCF8C / 8FE454 / 8FE924 / 8FFFE4
//   Calibration  : 48342-12, 41100060(A), 41100097   (full payload = 5 bytes after header)
static const AbsValueDef AbsMapB_Values[] = {
    {ABSV_FRONT_WHEEL_SPEED, 0, 1, 1.0f, 0.0f},
    {ABSV_REAR_WHEEL_SPEED, 1, 1, 1.0f, 0.0f},
    {ABSV_ABS_ECU_MODULE_VOLTAGE, 3, 1, 0.08f, 0.0f}
};
static const AbsStateDef AbsMapB_States[] = {
    {ABSS_REAR_WHEEL_SENSOR_FAULT, 4, 0x40},
    {ABSS_FRONT_WHEEL_SENSOR_FAULT, 4, 0x80},
    {ABSS_FRONT_BRAKE_SWITCH, 4, 0x20},
    {ABSS_REAR_BRAKE_SWITCH, 4, 0x10},
    {ABSS_ABS_ACTIVE, 4, 0x08},
    {ABSS_PUMP_MOTOR_STATUS, 2, 0x02},
    {ABSS_FRONT_INLET_STATUS, 2, 0x04},
    {ABSS_FRONT_OUTLET_STATUS, 2, 0x08},
    {ABSS_REAR_INLET_STATUS, 2, 0x10},
    {ABSS_REAR_OUTLET_STATUS, 2, 0x20}
};

// Family C: 8-bit + master-cylinder/caliper pressures (DEFAULT)
//   Database sets : 8FDEA4 / 8FEDF4 / 8FFA34 / 8FDAB4
//   Calibration  : 41100012, 41100062, 41100106, DEFAULT   (full payload = 19 bytes after header)
static const AbsValueDef AbsMapC_Values[] = {
    {ABSV_FRONT_WHEEL_SPEED, 0, 1, 1.0f, 0.0f},
    {ABSV_REAR_WHEEL_SPEED, 1, 1, 1.0f, 0.0f},
    {ABSV_ABS_ECU_MODULE_VOLTAGE, 3, 1, 0.08f, 0.0f},
    {ABSV_FRONT_MASTER_CYLINDER_PRESSURE, 11, 2, 3.125f, 0.0f},
    {ABSV_REAR_MASTER_CYLINDER_PRESSURE, 13, 2, 3.125f, 0.0f},
    {ABSV_FRONT_CALIPER_CIRCUIT_PRESSURE, 15, 2, 3.125f, 0.0f},
    {ABSV_REAR_CALIPER_CIRCUIT_PRESSURE, 17, 2, 3.125f, 0.0f}
};
static const AbsStateDef AbsMapC_States[] = {
    {ABSS_REAR_WHEEL_SENSOR_FAULT, 4, 0x40},
    {ABSS_FRONT_WHEEL_SENSOR_FAULT, 4, 0x80},
    {ABSS_FRONT_BRAKE_SWITCH, 4, 0x20},
    {ABSS_REAR_BRAKE_SWITCH, 4, 0x10},
    {ABSS_ABS_ACTIVE, 4, 0x08},
    {ABSS_PUMP_MOTOR_STATUS, 2, 0x02},
    {ABSS_FRONT_INLET_STATUS, 2, 0x04},
    {ABSS_FRONT_OUTLET_STATUS, 2, 0x08},
    {ABSS_REAR_INLET_STATUS, 2, 0x10},
    {ABSS_REAR_OUTLET_STATUS, 2, 0x20}
};

// Family D: 4-channel (rear left + rear right wheel)
//   Database sets : 8FF3A4
//   Calibration  : 41100089   (full payload = 20 bytes after header)
static const AbsValueDef AbsMapD_Values[] = {
    {ABSV_FRONT_WHEEL_SPEED, 0, 1, 1.0f, 0.0f},
    {ABSV_REAR_WHEEL_SPEED, 1, 1, 1.0f, 0.0f},
    {ABSV_REAR_RIGHT_WHEEL_SPEED, 19, 1, 1.0f, 0.0f},
    {ABSV_ABS_ECU_MODULE_VOLTAGE, 3, 1, 0.08f, 0.0f},
    {ABSV_FRONT_MASTER_CYLINDER_PRESSURE, 11, 2, 3.125f, 0.0f},
    {ABSV_REAR_MASTER_CYLINDER_PRESSURE, 13, 2, 3.125f, 0.0f},
    {ABSV_FRONT_CALIPER_CIRCUIT_PRESSURE, 15, 2, 3.125f, 0.0f},
    {ABSV_REAR_CALIPER_CIRCUIT_PRESSURE, 17, 2, 3.125f, 0.0f}
};
static const AbsStateDef AbsMapD_States[] = {
    {ABSS_FRONT_WHEEL_SENSOR_FAULT, 4, 0x80},
    {ABSS_REAR_WHEEL_SENSOR_FAULT, 4, 0x40},
    {ABSS_REAR_RIGHT_WHEEL_SENSOR_FAULT, 4, 0x01},
    {ABSS_FRONT_BRAKE_SWITCH, 4, 0x20},
    {ABSS_REAR_BRAKE_SWITCH, 4, 0x10},
    {ABSS_ABS_ACTIVE, 4, 0x08},
    {ABSS_PUMP_MOTOR_STATUS, 2, 0x02},
    {ABSS_FRONT_INLET_STATUS, 2, 0x04},
    {ABSS_FRONT_OUTLET_STATUS, 2, 0x08},
    {ABSS_REAR_INLET_STATUS, 2, 0x10},
    {ABSS_REAR_OUTLET_STATUS, 2, 0x20},
    {ABSS_REAR_RIGHT_INLET_STATUS, 2, 0x40},
    {ABSS_REAR_RIGHT_OUTLET_STATUS, 2, 0x80}
};

static const AbsFamilyMap ABS_FAMILY_MAPS[ABS_FAMILY_COUNT] = {
    {"A", AbsMapA_Values, (uint8_t)(sizeof(AbsMapA_Values)/sizeof(AbsMapA_Values[0])), AbsMapA_States, (uint8_t)(sizeof(AbsMapA_States)/sizeof(AbsMapA_States[0]))},
    {"B", AbsMapB_Values, (uint8_t)(sizeof(AbsMapB_Values)/sizeof(AbsMapB_Values[0])), AbsMapB_States, (uint8_t)(sizeof(AbsMapB_States)/sizeof(AbsMapB_States[0]))},
    {"C", AbsMapC_Values, (uint8_t)(sizeof(AbsMapC_Values)/sizeof(AbsMapC_Values[0])), AbsMapC_States, (uint8_t)(sizeof(AbsMapC_States)/sizeof(AbsMapC_States[0]))},
    {"D", AbsMapD_Values, (uint8_t)(sizeof(AbsMapD_Values)/sizeof(AbsMapD_Values[0])), AbsMapD_States, (uint8_t)(sizeof(AbsMapD_States)/sizeof(AbsMapD_States[0]))}
};

struct AbsCalRule { const char* prefix; AbsFamily family; };
static const AbsCalRule ABS_CAL_RULES[] = {
    {"40662-11", ABS_FAMILY_A}, {"41100036", ABS_FAMILY_A},
    {"48342-12", ABS_FAMILY_B}, {"41100060", ABS_FAMILY_B}, {"41100097", ABS_FAMILY_B},
    {"41100012", ABS_FAMILY_C}, {"41100062", ABS_FAMILY_C}, {"41100106", ABS_FAMILY_C},
    {"41100089", ABS_FAMILY_D}
};
#define ABS_CAL_RULE_COUNT (sizeof(ABS_CAL_RULES) / sizeof(ABS_CAL_RULES[0]))

// Classify an ABS calibration ID (e.g. "41100062") into an AbsFamily. No match -> ABS_FAMILY_DEFAULT.
static inline AbsFamily classifyAbsCalibrationId(const char* cal, const char** out_rule = nullptr) {
    if (out_rule) *out_rule = "default";
    if (cal == nullptr) return ABS_FAMILY_DEFAULT;
    while (*cal == ' ' || *cal == '\t') cal++;
    for (size_t i = 0; i < ABS_CAL_RULE_COUNT; i++) {
        if (strncmp(cal, ABS_CAL_RULES[i].prefix, strlen(ABS_CAL_RULES[i].prefix)) == 0) {
            if (out_rule) *out_rule = ABS_CAL_RULES[i].prefix;
            return ABS_CAL_RULES[i].family;
        }
    }
    return ABS_FAMILY_DEFAULT;
}

// ============================================================================
// 5C. BCM (0x7E2) DID 0x0200 FAMILY MAPS
//     BCM (Body Control Module) has five distinct DID 0x0200 layouts, chosen by the BCM Calibration ID
//     (DID 0xF1ED / 0xF193 read from 0x7E2). The longest matching ID prefix wins.
//     Offsets are 0-based bytes AFTER the 62 02 00 header; 16-bit values big-endian;
//     value = raw * scale; states are (byte & mask) == mask with per-state on/off wording.
//     Items beyond the received payload length are skipped.
// ============================================================================
enum BcmFamily : uint8_t { BCM_FAMILY_A = 0, BCM_FAMILY_B, BCM_FAMILY_C, BCM_FAMILY_D, BCM_FAMILY_E, BCM_FAMILY_COUNT };
#define BCM_FAMILY_DEFAULT        BCM_FAMILY_E   // the default family when no part-number prefix matches

enum BcmStateIdx : uint8_t {
    BCMS_OIL_PRESSURE_INPUT_LAMP, BCMS_REAR_BRAKE_SWITCH, BCMS_NEUTRAL_SWITCH_INPUT,
    BCMS_HI_BEAM_INPUT, BCMS_HORN_INPUT, BCMS_LEFT_TURN_SWITCH_INPUT,
    BCMS_HAZARD_SWITCH_INPUT, BCMS_CLUTCH_SWITCH_INPUT, BCMS_TRIP_SWITCH_STATUS,
    BCMS_RUN_MODE_FLAG, BCMS_START_INPUT, BCMS_RIGHT_TURN_SWITCH_INPUT,
    BCMS_FRONT_BRAKE_SWITCH, BCMS_SWITCHED_POWER, BCMS_ACCESSORY_POWER,
    BCMS_FUEL_PUMP, BCMS_LOW_BEAM, BCMS_HIGH_BEAM,
    BCMS_HORN, BCMS_REAR_RIGHT_TURN, BCMS_REAR_LEFT_TURN,
    BCMS_BRAKE_LAMP, BCMS_RUN_POSITION_LIGHTS, BCMS_FRONT_RIGHT_TURN,
    BCMS_SYSTEM_POWER, BCMS_FRONT_LEFT_TURN, BCMS_TIP_OVER_STATE,
    BCMS_SIDECAR_TRIGLIDE_MODE_FLAG, BCMS_STARTER_RELAY_ENABLE_OUTPUT, BCMS_ACCEL_AXIS,
    BCMS_LOW_BEAM_SWITCH, BCMS_AUX_POLICE_FRONT_LTS, BCMS_OFF_SWITCH_INPUT,
    BCMS_COOLANT_PUMP,
    BCMS_COUNT
};
static const char* const BCM_STATE_KEYS[BCMS_COUNT] = {
    "oil_pressure_input_lamp", "rear_brake_switch", "neutral_switch_input",
    "hi_beam_input", "horn_input", "left_turn_switch_input",
    "hazard_switch_input", "clutch_switch_input", "trip_switch_status",
    "run_mode_flag", "start_input", "right_turn_switch_input",
    "front_brake_switch", "switched_power", "accessory_power",
    "fuel_pump", "low_beam", "high_beam",
    "horn", "rear_right_turn", "rear_left_turn",
    "brake_lamp", "run_position_lights", "front_right_turn",
    "system_power", "front_left_turn", "tip_over_state",
    "sidecar_triglide_mode_flag", "starter_relay_enable_output", "accel_axis",
    "low_beam_switch", "aux_police_front_lts", "off_switch_input",
    "coolant_pump"
};
// {text when (byte & mask) == mask, text otherwise}
static const char* const BCM_STATE_TEXT[BCMS_COUNT][2] = {
    {"on", "off"},
    {"on", "off"},
    {"In Neutral", "Not In Neutral"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"Pulled In", "Released"},
    {"Pressed", "Released"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"on", "off"},
    {"Tipped", "Upright"},
    {"Yes", "No"},
    {"on", "off"},
    {"Z Axis", "Y Axis"},
    {"on", "off"},
    {"Active", "Inactive"},
    {"on", "off"},
    {"on", "off"}
};

struct BcmValueDef { uint8_t offset; uint8_t len; float scale; };    // system (battery) voltage
struct BcmStateDef { uint8_t idx; uint8_t offset; uint8_t mask; };
struct BcmFamilyMap {
    const char*        name;
    BcmValueDef        voltage;
    const BcmStateDef* states;  uint8_t state_count;
};

// Family A: calibration IDs 69991-11, 69991-11B, 69991-11C, 69992-12, 69993-11, 69993-11B ... (11 IDs)
static const BcmStateDef BcmMapA_States[] = {
    {BCMS_OIL_PRESSURE_INPUT_LAMP, 1, 0x04},
    {BCMS_REAR_BRAKE_SWITCH, 1, 0x10},
    {BCMS_NEUTRAL_SWITCH_INPUT, 1, 0x20},
    {BCMS_HI_BEAM_INPUT, 2, 0x02},
    {BCMS_HORN_INPUT, 2, 0x04},
    {BCMS_LEFT_TURN_SWITCH_INPUT, 2, 0x08},
    {BCMS_HAZARD_SWITCH_INPUT, 2, 0x10},
    {BCMS_CLUTCH_SWITCH_INPUT, 2, 0x20},
    {BCMS_TRIP_SWITCH_STATUS, 3, 0x01},
    {BCMS_RUN_MODE_FLAG, 3, 0x02},
    {BCMS_START_INPUT, 3, 0x04},
    {BCMS_RIGHT_TURN_SWITCH_INPUT, 3, 0x08},
    {BCMS_FRONT_BRAKE_SWITCH, 3, 0x10},
    {BCMS_SWITCHED_POWER, 4, 0x01},
    {BCMS_ACCESSORY_POWER, 4, 0x02},
    {BCMS_FUEL_PUMP, 4, 0x04},
    {BCMS_LOW_BEAM, 4, 0x10},
    {BCMS_HIGH_BEAM, 4, 0x20},
    {BCMS_HORN, 5, 0x01},
    {BCMS_REAR_RIGHT_TURN, 5, 0x02},
    {BCMS_REAR_LEFT_TURN, 5, 0x04},
    {BCMS_BRAKE_LAMP, 5, 0x08},
    {BCMS_RUN_POSITION_LIGHTS, 5, 0x10},
    {BCMS_FRONT_RIGHT_TURN, 5, 0x40},
    {BCMS_SYSTEM_POWER, 5, 0x80},
    {BCMS_FRONT_LEFT_TURN, 7, 0x80},
    {BCMS_TIP_OVER_STATE, 27, 0x20},
    {BCMS_SIDECAR_TRIGLIDE_MODE_FLAG, 27, 0x40}
};

// Family B: calibration IDs 69992-12A, 69992-12B, 69994-12A, 69994-12B, 69994-12C, 69996-12A ... (30 IDs)
static const BcmStateDef BcmMapB_States[] = {
    {BCMS_OIL_PRESSURE_INPUT_LAMP, 1, 0x04},
    {BCMS_REAR_BRAKE_SWITCH, 1, 0x10},
    {BCMS_NEUTRAL_SWITCH_INPUT, 1, 0x20},
    {BCMS_HI_BEAM_INPUT, 2, 0x02},
    {BCMS_HORN_INPUT, 2, 0x04},
    {BCMS_LEFT_TURN_SWITCH_INPUT, 2, 0x08},
    {BCMS_HAZARD_SWITCH_INPUT, 2, 0x10},
    {BCMS_CLUTCH_SWITCH_INPUT, 2, 0x20},
    {BCMS_TRIP_SWITCH_STATUS, 3, 0x01},
    {BCMS_RUN_MODE_FLAG, 3, 0x02},
    {BCMS_START_INPUT, 3, 0x04},
    {BCMS_RIGHT_TURN_SWITCH_INPUT, 3, 0x08},
    {BCMS_FRONT_BRAKE_SWITCH, 3, 0x10},
    {BCMS_SWITCHED_POWER, 4, 0x01},
    {BCMS_ACCESSORY_POWER, 4, 0x02},
    {BCMS_FUEL_PUMP, 4, 0x04},
    {BCMS_LOW_BEAM, 4, 0x10},
    {BCMS_HIGH_BEAM, 4, 0x20},
    {BCMS_HORN, 5, 0x01},
    {BCMS_REAR_RIGHT_TURN, 5, 0x02},
    {BCMS_REAR_LEFT_TURN, 5, 0x04},
    {BCMS_BRAKE_LAMP, 5, 0x08},
    {BCMS_RUN_POSITION_LIGHTS, 5, 0x10},
    {BCMS_FRONT_RIGHT_TURN, 5, 0x40},
    {BCMS_SYSTEM_POWER, 5, 0x80},
    {BCMS_FRONT_LEFT_TURN, 7, 0x80},
    {BCMS_TIP_OVER_STATE, 27, 0x20}
};

// Family C: calibration IDs 41000168, 41000169 (2 IDs)
static const BcmStateDef BcmMapC_States[] = {
    {BCMS_OIL_PRESSURE_INPUT_LAMP, 1, 0x04},
    {BCMS_REAR_BRAKE_SWITCH, 1, 0x10},
    {BCMS_NEUTRAL_SWITCH_INPUT, 1, 0x20},
    {BCMS_HI_BEAM_INPUT, 2, 0x02},
    {BCMS_HORN_INPUT, 2, 0x04},
    {BCMS_LEFT_TURN_SWITCH_INPUT, 2, 0x08},
    {BCMS_HAZARD_SWITCH_INPUT, 2, 0x10},
    {BCMS_CLUTCH_SWITCH_INPUT, 2, 0x20},
    {BCMS_TRIP_SWITCH_STATUS, 3, 0x01},
    {BCMS_RUN_MODE_FLAG, 3, 0x02},
    {BCMS_START_INPUT, 3, 0x04},
    {BCMS_RIGHT_TURN_SWITCH_INPUT, 3, 0x08},
    {BCMS_FRONT_BRAKE_SWITCH, 3, 0x10},
    {BCMS_ACCESSORY_POWER, 4, 0x02},
    {BCMS_FUEL_PUMP, 4, 0x04},
    {BCMS_STARTER_RELAY_ENABLE_OUTPUT, 4, 0x08},
    {BCMS_LOW_BEAM, 4, 0x10},
    {BCMS_HIGH_BEAM, 4, 0x20},
    {BCMS_HORN, 5, 0x01},
    {BCMS_REAR_RIGHT_TURN, 5, 0x02},
    {BCMS_REAR_LEFT_TURN, 5, 0x04},
    {BCMS_BRAKE_LAMP, 5, 0x08},
    {BCMS_RUN_POSITION_LIGHTS, 5, 0x10},
    {BCMS_FRONT_RIGHT_TURN, 5, 0x40},
    {BCMS_SYSTEM_POWER, 5, 0x80},
    {BCMS_FRONT_LEFT_TURN, 7, 0x80},
    {BCMS_ACCEL_AXIS, 27, 0x10},
    {BCMS_TIP_OVER_STATE, 27, 0x20},
    {BCMS_LOW_BEAM_SWITCH, 2, 0x01},
    {BCMS_AUX_POLICE_FRONT_LTS, 4, 0x01}
};

// Family D: calibration IDs 41000663, 41000662 (2 IDs)
static const BcmStateDef BcmMapD_States[] = {
    {BCMS_REAR_BRAKE_SWITCH, 1, 0x10},
    {BCMS_HI_BEAM_INPUT, 2, 0x02},
    {BCMS_HORN_INPUT, 2, 0x04},
    {BCMS_LEFT_TURN_SWITCH_INPUT, 2, 0x08},
    {BCMS_HAZARD_SWITCH_INPUT, 2, 0x10},
    {BCMS_TRIP_SWITCH_STATUS, 3, 0x01},
    {BCMS_RUN_MODE_FLAG, 3, 0x02},
    {BCMS_START_INPUT, 3, 0x04},
    {BCMS_RIGHT_TURN_SWITCH_INPUT, 3, 0x08},
    {BCMS_FRONT_BRAKE_SWITCH, 3, 0x10},
    {BCMS_SWITCHED_POWER, 4, 0x01},
    {BCMS_ACCESSORY_POWER, 4, 0x02},
    {BCMS_LOW_BEAM, 4, 0x10},
    {BCMS_HIGH_BEAM, 4, 0x20},
    {BCMS_HORN, 5, 0x01},
    {BCMS_REAR_RIGHT_TURN, 5, 0x02},
    {BCMS_REAR_LEFT_TURN, 5, 0x04},
    {BCMS_BRAKE_LAMP, 5, 0x08},
    {BCMS_RUN_POSITION_LIGHTS, 5, 0x10},
    {BCMS_FRONT_RIGHT_TURN, 5, 0x40},
    {BCMS_SYSTEM_POWER, 5, 0x80},
    {BCMS_FRONT_LEFT_TURN, 7, 0x80},
    {BCMS_TIP_OVER_STATE, 27, 0x20},
    {BCMS_OFF_SWITCH_INPUT, 3, 0x20},
    {BCMS_COOLANT_PUMP, 4, 0x08}
};

// Family E: calibration IDs 41000674, 41000675, 41000678, 41000679, 41001169, 41001170 ... (14 IDs)
static const BcmStateDef BcmMapE_States[] = {
    {BCMS_REAR_BRAKE_SWITCH, 0, 0x20},
    {BCMS_NEUTRAL_SWITCH_INPUT, 0, 0x40},
    {BCMS_FUEL_PUMP, 2, 0x10},
    {BCMS_LOW_BEAM, 3, 0x08},
    {BCMS_HIGH_BEAM, 3, 0x04},
    {BCMS_HORN, 3, 0x20},
    {BCMS_REAR_RIGHT_TURN, 3, 0x02},
    {BCMS_REAR_LEFT_TURN, 3, 0x01},
    {BCMS_RUN_POSITION_LIGHTS, 3, 0x40},
    {BCMS_FRONT_RIGHT_TURN, 2, 0x80},
    {BCMS_FRONT_LEFT_TURN, 2, 0x40},
    {BCMS_TIP_OVER_STATE, 47, 0x20}
};

static const BcmFamilyMap BCM_FAMILY_MAPS[BCM_FAMILY_COUNT] = {
    {"A", {16, 1, 0.1f}, BcmMapA_States, (uint8_t)(sizeof(BcmMapA_States)/sizeof(BcmMapA_States[0]))},
    {"B", {16, 1, 0.1f}, BcmMapB_States, (uint8_t)(sizeof(BcmMapB_States)/sizeof(BcmMapB_States[0]))},
    {"C", {16, 1, 0.1f}, BcmMapC_States, (uint8_t)(sizeof(BcmMapC_States)/sizeof(BcmMapC_States[0]))},
    {"D", {16, 1, 0.1f}, BcmMapD_States, (uint8_t)(sizeof(BcmMapD_States)/sizeof(BcmMapD_States[0]))},
    {"E", {6, 2, 0.0004f}, BcmMapE_States, (uint8_t)(sizeof(BcmMapE_States)/sizeof(BcmMapE_States[0]))}
};

struct BcmCalRule { const char* prefix; BcmFamily family; };
static const BcmCalRule BCM_CAL_RULES[] = {
    {"69991-11", BCM_FAMILY_A},
    {"69991-11B", BCM_FAMILY_A},
    {"69991-11C", BCM_FAMILY_A},
    {"69992-12", BCM_FAMILY_A},
    {"69993-11", BCM_FAMILY_A},
    {"69993-11B", BCM_FAMILY_A},
    {"69993-11C", BCM_FAMILY_A},
    {"69994-12", BCM_FAMILY_A},
    {"69996-12", BCM_FAMILY_A},
    {"69997-11B", BCM_FAMILY_A},
    {"69997-11C", BCM_FAMILY_A},
    {"69992-12A", BCM_FAMILY_B},
    {"69992-12B", BCM_FAMILY_B},
    {"69994-12A", BCM_FAMILY_B},
    {"69994-12B", BCM_FAMILY_B},
    {"69994-12C", BCM_FAMILY_B},
    {"69996-12A", BCM_FAMILY_B},
    {"69996-12B", BCM_FAMILY_B},
    {"41000341", BCM_FAMILY_B},
    {"41000342", BCM_FAMILY_B},
    {"41000349", BCM_FAMILY_B},
    {"41000350", BCM_FAMILY_B},
    {"41000341B", BCM_FAMILY_B},
    {"41000342B", BCM_FAMILY_B},
    {"41000348", BCM_FAMILY_B},
    {"41000348B", BCM_FAMILY_B},
    {"41000349B", BCM_FAMILY_B},
    {"41000350B", BCM_FAMILY_B},
    {"41000351", BCM_FAMILY_B},
    {"41000351B", BCM_FAMILY_B},
    {"41000341E", BCM_FAMILY_B},
    {"41000342E", BCM_FAMILY_B},
    {"41000349E", BCM_FAMILY_B},
    {"41000350E", BCM_FAMILY_B},
    {"41000351E", BCM_FAMILY_B},
    {"41000351F", BCM_FAMILY_B},
    {"41000350F", BCM_FAMILY_B},
    {"41000349F", BCM_FAMILY_B},
    {"41000348F", BCM_FAMILY_B},
    {"41000342F", BCM_FAMILY_B},
    {"41000341F", BCM_FAMILY_B},
    {"41000168", BCM_FAMILY_C},
    {"41000169", BCM_FAMILY_C},
    {"41000663", BCM_FAMILY_D},
    {"41000662", BCM_FAMILY_D},
    {"41000674", BCM_FAMILY_E},
    {"41000675", BCM_FAMILY_E},
    {"41000678", BCM_FAMILY_E},
    {"41000679", BCM_FAMILY_E},
    {"41001169", BCM_FAMILY_E},
    {"41001170", BCM_FAMILY_E},
    {"41001171", BCM_FAMILY_E},
    {"41001172", BCM_FAMILY_E},
    {"41001169A", BCM_FAMILY_E},
    {"41001170A", BCM_FAMILY_E},
    {"41001171A", BCM_FAMILY_E},
    {"41001172A", BCM_FAMILY_E}
};
#define BCM_CAL_RULE_COUNT (sizeof(BCM_CAL_RULES) / sizeof(BCM_CAL_RULES[0]))

// Classify a BCM calibration ID (e.g. "41000674") into a BcmFamily. Longest matching prefix wins
// ("69992-12A" beats "69992-12"); no match -> BCM_FAMILY_DEFAULT.
static inline BcmFamily classifyBcmCalibrationId(const char* cal, const char** out_rule = nullptr) {
    if (out_rule) *out_rule = "default";
    if (cal == nullptr) return BCM_FAMILY_DEFAULT;
    while (*cal == ' ' || *cal == '\t') cal++;
    size_t best_len = 0;
    BcmFamily best = BCM_FAMILY_DEFAULT;
    for (size_t i = 0; i < BCM_CAL_RULE_COUNT; i++) {
        size_t n = strlen(BCM_CAL_RULES[i].prefix);
        if (n > best_len && strncmp(cal, BCM_CAL_RULES[i].prefix, n) == 0) {
            best_len = n; best = BCM_CAL_RULES[i].family;
            if (out_rule) *out_rule = BCM_CAL_RULES[i].prefix;
        }
    }
    return best;
}

// ============================================================================
// 5D. GENERIC MODULE DID 0x0200 DECODE (SCM, LHC, RHC, EHCU, SPEEDO) - generated decode table
// ============================================================================
// Each module has one or more layouts selected by calibration-ID prefix (longest prefix wins).
// Item kinds: GEN_VAL = big-endian unsigned value * scale, GEN_FLAG = (byte & mask) != 0.
enum GenKind : uint8_t { GEN_VAL = 0, GEN_FLAG = 1 };
struct GenItem { uint8_t key; uint8_t kind; uint8_t nbytes; uint8_t pos; uint8_t mask; float scale; };
struct GenLayout { const char* name; const GenItem* items; uint8_t count; };
struct GenCalRule { const char* prefix; uint8_t layout; };
struct GenModuleDef { uint16_t id; const char* name; const GenLayout* layouts; uint8_t layout_count; const GenCalRule* rules; uint8_t rule_count; uint8_t default_layout; };
static const char* const GEN_KEYS[] = {
    "battery_voltage",
    "security_state",
    "siren",
    "horn_switch",
    "high_beam_switch",
    "clutch_switch",
    "hazard_switch",
    "start_switch",
    "right_turn_switch",
    "front_brake_switch",
    "engine_off_switch",
    "engine_run_switch",
    "front_wheel_speed",
    "rear_wheel_speed",
    "rear_wheel_speed_sensor_fault",
    "front_wheel_speed_sensor_fault",
    "rear_brake_switch",
    "pump_motor_status",
    "front_inlet_status",
    "front_outlet_status",
    "rear_inlet_status",
    "rear_outlet_status",
    "abs_active",
    "front_caliper_circuit_pressure",
    "front_master_cylinder_pressure",
    "rear_master_cylinder_pressure",
    "rear_caliper_circuit_pressure",
    "battery_voltage_speedometer",
    "odometer_km",
    "fuel_sender_volts",
    "ambient_light_sensor",
    "fuel_level_percent",
    "trip_a_displayed",
    "trip_b_displayed",
    "check_engine_lamp",
    "battery_charge_telltale",
    "6th_gear_lamp",
    "low_fuel_telltale",
    "high_beam_indicator",
    "oil_telltale",
    "neutral_indicator_lamp",
    "right_turn_indicator",
    "left_turn_indicator",
    "trip_keyon_displayed",
    "left_turn_switch_input",
    "traction_control_telltale",
    "powertrain_warning",
    "powertrain_fault",
    "tpms_telltale",
    "12v_battery_telltale",
    "service_telltale",
    "check_engine_regulatory_mil_telltale",
    "fil_telltale",
    "coolant_temp_telltale",
    "abs_telltale",
    "traction_control_fault_telltale",
    "left_turn_telltale",
    "right_turn_telltale",
    "low_tire_pressure_telltale",
    "lcd_intensity_level",
    "remaining_miles_to_marry",
    "telltale_intensity_level",
    "low_fuel_trip_meter",
    "bluetooth_status_telltale",
    "coolant_telltale",
    "high_beam_telltale",
    "neutral_telltale",
    "oil_pressure_telltale",
    "cruise_enabled_telltale_amber",
    "cruise_enabled_telltale_green",
    "rear_abs_disabled_telltale",
    "park_brake_telltale",
    "heated_grip_telltale",
    "shift_detection_telltale",
    "vehicle_hold_control_telltale",
    "variable_ride_height_telltale",
    "fog_lamps_aux_light",
    "ride_mode_telltale",
    "road_ride_mode_telltale",
    "rain_ride_mode_telltale",
    "sport_ride_mode_telltale"
};
#define GEN_KEY_COUNT 81
static const GenItem GEN_SCM_L0_ITEMS[] = {
    {0,0,2,11,0,0.01f},
    {1,1,1,32,1,1.0f},
    {2,1,1,1,16,1.0f}
};
static const GenLayout GEN_SCM_LAYOUTS[] = {
    {"SCM-A", GEN_SCM_L0_ITEMS, 3}
};
static const GenItem GEN_LHC_L0_ITEMS[] = {
    {3,1,1,0,2,1.0f},
    {4,1,1,0,4,1.0f},
    {5,1,1,0,64,1.0f}
};
static const GenLayout GEN_LHC_LAYOUTS[] = {
    {"LHC-A", GEN_LHC_L0_ITEMS, 3}
};
static const GenItem GEN_RHC_L0_ITEMS[] = {
    {6,1,1,0,1,1.0f},
    {7,1,1,0,2,1.0f},
    {8,1,1,0,32,1.0f},
    {9,1,1,0,64,1.0f},
    {10,1,1,0,4,1.0f},
    {11,1,1,0,8,1.0f}
};
static const GenLayout GEN_RHC_LAYOUTS[] = {
    {"RHC-A", GEN_RHC_L0_ITEMS, 6}
};
static const GenItem GEN_EHCU_L0_ITEMS[] = {
    {12,0,1,0,0,1.0f},
    {13,0,1,1,0,1.0f},
    {0,0,1,3,0,0.08f},
    {14,1,1,4,64,1.0f},
    {15,1,1,4,128,1.0f},
    {9,1,1,4,32,1.0f},
    {16,1,1,4,16,1.0f},
    {17,1,1,2,2,1.0f},
    {18,1,1,2,4,1.0f},
    {19,1,1,2,8,1.0f},
    {20,1,1,2,16,1.0f},
    {21,1,1,2,32,1.0f},
    {22,1,1,4,8,1.0f}
};
static const GenItem GEN_EHCU_L1_ITEMS[] = {
    {12,0,1,0,0,1.0f},
    {13,0,1,1,0,1.0f},
    {0,0,1,3,0,0.08f},
    {23,0,2,15,0,3.125f},
    {14,1,1,4,64,1.0f},
    {15,1,1,4,128,1.0f},
    {9,1,1,4,32,1.0f},
    {17,1,1,2,2,1.0f},
    {18,1,1,2,4,1.0f},
    {19,1,1,2,8,1.0f},
    {20,1,1,2,16,1.0f},
    {21,1,1,2,32,1.0f},
    {22,1,1,4,8,1.0f}
};
static const GenItem GEN_EHCU_L2_ITEMS[] = {
    {12,0,1,0,0,1.0f},
    {13,0,1,1,0,1.0f},
    {0,0,1,3,0,0.08f},
    {24,0,2,11,0,3.125f},
    {25,0,2,13,0,3.125f},
    {23,0,2,15,0,3.125f},
    {26,0,2,17,0,3.125f},
    {14,1,1,4,64,1.0f},
    {15,1,1,4,128,1.0f},
    {9,1,1,4,32,1.0f},
    {16,1,1,4,16,1.0f},
    {17,1,1,2,2,1.0f},
    {18,1,1,2,4,1.0f},
    {19,1,1,2,8,1.0f},
    {20,1,1,2,16,1.0f},
    {21,1,1,2,32,1.0f},
    {22,1,1,4,8,1.0f}
};
static const GenLayout GEN_EHCU_LAYOUTS[] = {
    {"EHCU-A", GEN_EHCU_L0_ITEMS, 13},
    {"EHCU-B", GEN_EHCU_L1_ITEMS, 13},
    {"EHCU-C", GEN_EHCU_L2_ITEMS, 17}
};
static const GenCalRule GEN_EHCU_RULES[] = {
    {"41100110", 0},
    {"41100130", 0},
    {"41100112", 1},
    {"41100123", 2}
};
static const GenItem GEN_SPDO_L0_ITEMS[] = {
    {27,0,2,1,0,0.001f},
    {28,0,4,61,0,0.001f},
    {29,0,2,3,0,0.001f},
    {30,0,2,5,0,0.0196f},
    {31,0,2,20,0,0.001526f},
    {32,0,4,45,0,0.001f},
    {33,0,4,53,0,0.001f},
    {34,1,1,29,1,1.0f},
    {35,1,1,29,128,1.0f},
    {36,1,1,34,1,1.0f},
    {37,1,1,30,1,1.0f},
    {38,1,1,30,2,1.0f},
    {39,1,1,30,4,1.0f},
    {40,1,1,30,8,1.0f},
    {41,1,1,30,16,1.0f},
    {42,1,1,30,32,1.0f}
};
static const GenItem GEN_SPDO_L1_ITEMS[] = {
    {27,0,2,1,0,0.001f},
    {28,0,4,61,0,0.001f},
    {29,0,2,3,0,0.001f},
    {30,0,2,5,0,0.0196f},
    {31,0,2,20,0,0.001526f},
    {32,0,4,45,0,0.001f},
    {33,0,4,53,0,0.001f},
    {34,1,1,29,1,1.0f},
    {35,1,1,29,128,1.0f},
    {37,1,1,30,1,1.0f},
    {38,1,1,30,2,1.0f},
    {39,1,1,30,4,1.0f},
    {40,1,1,30,8,1.0f},
    {41,1,1,30,16,1.0f},
    {42,1,1,30,32,1.0f}
};
static const GenItem GEN_SPDO_L2_ITEMS[] = {
    {27,0,2,1,0,0.001f},
    {28,0,4,61,0,0.001f},
    {30,0,2,5,0,0.0196f},
    {32,0,4,45,0,0.001f},
    {33,0,4,53,0,0.001f},
    {34,1,1,29,1,1.0f},
    {35,1,1,29,128,1.0f},
    {37,1,1,30,1,1.0f},
    {38,1,1,30,2,1.0f},
    {39,1,1,30,4,1.0f},
    {40,1,1,30,8,1.0f},
    {41,1,1,30,16,1.0f},
    {42,1,1,30,32,1.0f}
};
static const GenItem GEN_SPDO_L3_ITEMS[] = {
    {32,0,4,28,0,0.001f},
    {33,0,4,36,0,0.001f},
    {43,0,4,50,0,0.001f},
    {34,1,1,0,1,1.0f},
    {4,1,1,0,2,1.0f},
    {44,1,1,0,4,1.0f},
    {8,1,1,0,8,1.0f},
    {22,1,1,0,16,1.0f},
    {45,1,1,0,32,1.0f},
    {46,1,1,0,64,1.0f},
    {47,1,1,0,128,1.0f},
    {48,1,1,0,128,1.0f}
};
static const GenItem GEN_SPDO_L4_ITEMS[] = {
    {32,0,4,10,0,0.001f},
    {33,0,4,18,0,0.001f},
    {28,0,4,77,0,0.001f},
    {49,1,1,0,1,1.0f},
    {50,1,1,0,4,1.0f},
    {51,1,1,0,8,1.0f},
    {52,1,1,0,32,1.0f},
    {38,1,1,0,64,1.0f},
    {53,1,1,1,16,1.0f},
    {54,1,1,1,32,1.0f},
    {45,1,1,1,64,1.0f},
    {55,1,1,1,128,1.0f},
    {56,1,1,2,2,1.0f},
    {57,1,1,2,4,1.0f},
    {58,1,1,2,8,1.0f}
};
static const GenItem GEN_SPDO_L5_ITEMS[] = {
    {32,0,4,10,0,0.001f},
    {33,0,4,18,0,0.001f},
    {28,0,4,77,0,0.001f}
};
static const GenItem GEN_SPDO_L6_ITEMS[] = {
    {28,0,4,61,0,0.001f},
    {30,0,2,5,0,0.0196f},
    {27,0,2,1,0,0.001f},
    {59,0,1,36,0,0.3922f},
    {60,0,2,65,0,1.0f},
    {61,0,1,80,0,0.3922f},
    {62,0,4,95,0,0.001f},
    {49,1,1,29,1,1.0f},
    {63,1,1,29,2,1.0f},
    {50,1,1,29,4,1.0f},
    {51,1,1,29,8,1.0f},
    {64,1,1,29,16,1.0f},
    {52,1,1,29,32,1.0f},
    {65,1,1,29,64,1.0f},
    {66,1,1,29,128,1.0f},
    {37,1,1,30,1,1.0f},
    {67,1,1,30,4,1.0f},
    {68,1,1,30,4,1.0f},
    {69,1,1,30,8,1.0f},
    {53,1,1,30,16,1.0f},
    {54,1,1,30,32,1.0f},
    {45,1,1,30,64,1.0f},
    {55,1,1,30,128,1.0f},
    {70,1,1,31,1,1.0f},
    {42,1,1,31,32,1.0f},
    {41,1,1,31,16,1.0f},
    {58,1,1,31,8,1.0f},
    {71,1,1,31,16,1.0f},
    {72,1,1,31,32,1.0f},
    {73,1,1,31,64,1.0f},
    {74,1,1,31,128,1.0f},
    {75,1,1,32,1,1.0f},
    {76,1,1,32,2,1.0f},
    {77,1,1,32,4,1.0f},
    {78,1,1,32,32,1.0f},
    {79,1,1,32,32,1.0f},
    {80,1,1,32,128,1.0f}
};
static const GenLayout GEN_SPDO_LAYOUTS[] = {
    {"SPDO-A", GEN_SPDO_L0_ITEMS, 16},
    {"SPDO-B", GEN_SPDO_L1_ITEMS, 15},
    {"SPDO-C", GEN_SPDO_L2_ITEMS, 13},
    {"SPDO-D", GEN_SPDO_L3_ITEMS, 12},
    {"SPDO-E", GEN_SPDO_L4_ITEMS, 15},
    {"SPDO-F", GEN_SPDO_L5_ITEMS, 3},
    {"SPDO-G", GEN_SPDO_L6_ITEMS, 37}
};
static const GenCalRule GEN_SPDO_RULES[] = {
    {"67263-11", 0},
    {"67264-11", 0},
    {"70701-11", 0},
    {"70702-11", 0},
    {"70708-11", 0},
    {"74774-11", 0},
    {"74775-11", 0},
    {"74776-11", 0},
    {"74777-11", 0},
    {"70900100", 0},
    {"70900100A", 0},
    {"70900275", 0},
    {"70900275A", 0},
    {"70900369", 0},
    {"70900370", 0},
    {"70900371", 0},
    {"70900523", 0},
    {"70900524", 0},
    {"70900118D", 0},
    {"70900119C", 0},
    {"70900120C", 0},
    {"70900121D", 0},
    {"70900122C", 0},
    {"70900123C", 0},
    {"70900124D", 0},
    {"70900125C", 0},
    {"70900126C", 0},
    {"70900260C", 0},
    {"70900261B", 0},
    {"70900379C", 0},
    {"70900421C", 0},
    {"70900616B", 0},
    {"70900773B", 0},
    {"70900777B", 0},
    {"70900778B", 0},
    {"70900987", 0},
    {"70900988", 0},
    {"70900989", 0},
    {"70900121", 1},
    {"70900124", 1},
    {"70900293", 1},
    {"70900294", 1},
    {"70900295", 1},
    {"STREET", 1},
    {"70900644", 2},
    {"70900645", 2},
    {"70900646", 2},
    {"70901202", 2},
    {"70901203", 2},
    {"70901204", 2},
    {"SERENGETI", 2},
    {"70900801", 3},
    {"70901100", 3},
    {"70901105", 4},
    {"70901201", 5},
    {"70900971", 6},
    {"70900972", 6},
    {"70900973", 6},
    {"70901208", 6},
    {"70901209", 6},
    {"70901210", 6},
    {"70901218", 6},
    {"70901219", 6},
    {"70901220", 6}
};
#define GEN_MAX_ITEMS 37
static const GenModuleDef GEN_MODULES[] = {
    {0x7D0, "SCM", GEN_SCM_LAYOUTS, 1, nullptr, 0, 0},
    {0x7E3, "LHC", GEN_LHC_LAYOUTS, 1, nullptr, 0, 0},
    {0x7E5, "RHC", GEN_RHC_LAYOUTS, 1, nullptr, 0, 0},
    {0x7E7, "EHCU", GEN_EHCU_LAYOUTS, 3, GEN_EHCU_RULES, 4, 2},
    {0x7F2, "SPDO", GEN_SPDO_LAYOUTS, 7, GEN_SPDO_RULES, 64, 0}
};
#define GEN_MODULE_COUNT 5
static inline const GenModuleDef* findGenModule(uint32_t id) {
    for (uint8_t i = 0; i < GEN_MODULE_COUNT; i++) if (GEN_MODULES[i].id == id) return &GEN_MODULES[i];
    return nullptr;
}
// Longest-prefix calibration match; falls back to the module default layout.
static inline uint8_t classifyGenCalibrationId(const GenModuleDef* m, const char* cal, const char** rule_out) {
    if (rule_out) *rule_out = nullptr;
    if (!m) return 0;
    size_t best = 0; uint8_t layout = m->default_layout;
    if (cal) for (uint8_t i = 0; i < m->rule_count; i++) {
        size_t n = strlen(m->rules[i].prefix);
        if (n > best && strncmp(cal, m->rules[i].prefix, n) == 0) { best = n; layout = m->rules[i].layout; if (rule_out) *rule_out = m->rules[i].prefix; }
    }
    return layout;
}


// ============================================================================
// 6. FREERTOS DUAL-CORE TASK AFFINITY & STACK SIZES
// ============================================================================
#define CORE_CAN_DIAG_WORKER   0
#define CORE_SYSTEM_WEB_OTA    1
#define TASK_STACK_DIAG        12288
#define TASK_STACK_SUPERVISOR  8192

// ============================================================================
// 7. RUNTIME CONFIGURATION & SHARED TELEMETRY STRUCTURES
// ============================================================================
struct TesterSettings {
    char     node_name[48];
    bool     wifi_ap_mode;
    char     wifi_ssid[33];
    char     wifi_pass[65];
    char     ap_ssid[33];
    char     ap_pass[65];
    uint32_t can_bitrate;
    uint8_t  can_chip_type; // CanChipType: CAN_CHIP_TJA1043 or CAN_CHIP_PLAIN
    uint8_t  can_tx_pin;
    uint8_t  can_rx_pin;
    uint8_t  tja_stb_pin;
    uint8_t  tja_en_pin;
    uint8_t  tja_err_pin;
    uint8_t  tja_led_pin;
    bool     tja_sleep_enable;
    uint32_t tja_sleep_timeout_sec;
    uint32_t active_module_id;
    char     active_module_name[24];
    bool     safeboot_requested;
    bool     canlog_enabled;   // print CAN/UDS traffic ([UDS-TX]/[UDS-RX]/[ISOTP]/[UDS-SCAN]) to the serial
                               // console; default false (quiet) so the log can't flood or disturb operations
    bool     devtest_enabled; // show "Continue (debug)" on guided-procedure brake-pressure gates (ABS bleed
                               // press/release/pumpwait), letting the operator skip the live pressure check
                               // on a bench/no-bike setup; default false (off) so the real check always applies
};

struct SystemMetrics {
    uint32_t can_rx_count;
    uint32_t can_tx_count;
    uint32_t can_err_count;
    uint32_t uds_queries_count;
    uint32_t uds_replies_count;
    uint8_t  twai_state; // 0=STOPPED, 1=RUNNING, 2=BUS_OFF, 3=RECOVERING
    bool     tja_fault;
    bool     tja_sleeping;
    bool     tester_present_active;
    uint32_t tester_present_target;
    uint32_t last_can_activity_ms;
};

extern TesterSettings    settings;
extern SystemMetrics     metrics;
extern SemaphoreHandle_t metricsMutex;
