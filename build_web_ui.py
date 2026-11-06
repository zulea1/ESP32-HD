#!/usr/bin/env python3
# ==============================================================================
# PlatformIO Pre-Build Script: build_web_ui.py
# Project   : ESP32 HD - UDS Diagnostic Service Tester (ISO 14229 / ISO 15765-2)
# Copyright : (c) 2026 Henzulea Daniel email: <zulea1@gmail.com>
# License   : GNU General Public License v3.0 or later (GPL-3.0-or-later) - see LICENSE
# Version   : see FIRMWARE_VERSION_STR / FIRMWARE_BUILD_NUM_STR in src/core/config.h
# ==============================================================================
# Purpose:
#   Automatically scans the "web_src/" directory prior to C++ compilation, reads
#   all raw frontend source files (.html, .css, .js), and generates corresponding
#   C++ PROGMEM header files inside "src/web_ui/" wrapped in C++11 raw string
#   byte arrays: gzip-compressed assets, served with Content-Encoding: gzip.
#
# How Filename-to-Macro Mapping Works:
#   1. Each file discovered in "web_src/" (e.g., "index.html", "style.css",
#      "main.js", "safeboot.html") is stripped of its directory path.
#   2. Any dots ('.') and hyphens ('-') in the filename are replaced with
#      underscores ('_') to form a valid C/C++ identifier base:
#        - "index.html"    -> base: "index_html"
#        - "style.css"     -> base: "style_css"
#        - "main.js"       -> base: "main_js"
#        - "safeboot.html" -> base: "safeboot_html"
#   3. The output C++ header filename is "<base_lowercase>.h" inside "src/web_ui/":
#        - "src/web_ui/index_html.h"
#        - "src/web_ui/style_css.h"
#        - "src/web_ui/main_js.h"
#        - "src/web_ui/safeboot_html.h"
#   4. The PROGMEM constant variable identifier is "<BASE_UPPERCASE>":
#        - const char INDEX_HTML[] PROGMEM = R"rawliteral(...)rawliteral";
#        - const char STYLE_CSS[] PROGMEM = R"rawliteral(...)rawliteral";
#        - const char MAIN_JS[] PROGMEM = R"rawliteral(...)rawliteral";
#        - const char SAFEBOOT_HTML[] PROGMEM = R"rawliteral(...)rawliteral";
# ==============================================================================

import gzip
import os
import re
import time

# Hook into PlatformIO / SCons build environment when invoked via extra_scripts
IS_SERIAL_UPLOAD = False
try:
    Import("env", "COMMAND_LINE_TARGETS")
    PROJECT_DIR = env.subst("$PROJECT_DIR")
    if "upload" in COMMAND_LINE_TARGETS:
        IS_SERIAL_UPLOAD = True
except Exception:
    try:
        Import("env")
        PROJECT_DIR = env.subst("$PROJECT_DIR")
    except Exception:
        # Fallback when executed standalone from the command line (python3 build_web_ui.py)
        PROJECT_DIR = os.path.dirname(os.path.abspath(__file__))

WEB_SRC_DIR = os.path.join(PROJECT_DIR, "web_src")
WEB_OUT_DIR = os.path.join(PROJECT_DIR, "src", "web_ui")
STAMP_FILE = os.path.join(WEB_OUT_DIR, ".serial_flash_stamp")
CONFIG_H = os.path.join(PROJECT_DIR, "src", "core", "config.h")


def read_firmware_identity() -> dict:
    """
    Single source of truth: parses FIRMWARE_VERSION_STR / FIRMWARE_BUILD_NUM from src/core/config.h.
    FIRMWARE_BUILD_NUM_STR is only a preprocessor stringification of FIRMWARE_BUILD_NUM (one
    number to update, not two), so this reads the integer and formats it the same way the C
    preprocessor would. The web assets contain @@FW_VERSION@@ / @@FW_BUILD@@ placeholders that
    are replaced with these values when headers are generated.
    """
    with open(CONFIG_H, "r", encoding="utf-8") as f:
        text = f.read()
    ver = re.search(r'^\s*#define\s+FIRMWARE_VERSION_STR\s+"([^"]*)"', text, re.M)
    # Build label: prefer the free-form FIRMWARE_BUILD_NUM_STR string literal (e.g. "02a"); fall back to the
    # numeric FIRMWARE_BUILD_NUM if the label is still the preprocessor stringification macro.
    bld_str = re.search(r'^\s*#define\s+FIRMWARE_BUILD_NUM_STR\s+"([^"]*)"', text, re.M)
    bld_num = re.search(r'^\s*#define\s+FIRMWARE_BUILD_NUM\s+(\d+)', text, re.M)
    if not ver or (not bld_str and not bld_num):
        raise RuntimeError("[build_web_ui.py] FIRMWARE_VERSION_STR / FIRMWARE_BUILD_NUM(_STR) not found in src/core/config.h")
    build = bld_str.group(1) if bld_str else bld_num.group(1)
    return {"@@FW_VERSION@@": ver.group(1), "@@FW_BUILD@@": build}


def read_safeboot_identity() -> dict:
    """
    Safe Boot firmware carries its own version/build (SAFEBOOT_VERSION_STR / SAFEBOOT_BUILD_NUM_STR in
    config.h), independent of the main app. Used only for safeboot.html's @@FW_VERSION@@/@@FW_BUILD@@.
    Falls back to the main identity if the Safe Boot defines are absent.
    """
    with open(CONFIG_H, "r", encoding="utf-8") as f:
        text = f.read()
    ver = re.search(r'^\s*#define\s+SAFEBOOT_VERSION_STR\s+"([^"]*)"', text, re.M)
    bld = re.search(r'^\s*#define\s+SAFEBOOT_BUILD_NUM_STR\s+"([^"]*)"', text, re.M)
    if not ver or not bld:
        return read_firmware_identity()
    return {"@@FW_VERSION@@": ver.group(1), "@@FW_BUILD@@": bld.group(1)}


def get_serial_flash_stamp() -> str:
    """
    Generates a new flash stamp whenever 'upload' (serial flashing) is executed
    so the ESP32 performs a full NVS erase and resets to CONSTANTS on first boot.
    """
    os.makedirs(WEB_OUT_DIR, exist_ok=True)
    if IS_SERIAL_UPLOAD or not os.path.exists(STAMP_FILE):
        stamp = f"FLASH_{int(time.time())}"
        with open(STAMP_FILE, "w", encoding="utf-8") as sf:
            sf.write(stamp)
        return stamp
    with open(STAMP_FILE, "r", encoding="utf-8") as sf:
        return sf.read().strip() or "FLASH_INIT"


def sanitize_identifier(filename: str) -> str:
    """
    Converts a filename such as 'index.html' or 'style.css' into a valid
    C++ identifier base by replacing non-alphanumeric characters with '_'.
    """
    return re.sub(r"[^a-zA-Z0-9_]", "_", filename)


def generate_web_headers() -> None:
    """
    Scans 'web_src/', reads each file as UTF-8 text, wraps the content in
    R"rawliteral(...)rawliteral", and writes the header into 'src/web_ui/'.
    """
    if not os.path.exists(WEB_SRC_DIR):
        print(f"[build_web_ui.py] WARNING: Source directory not found: {WEB_SRC_DIR}")
        return

    # Ensure destination folder 'src/web_ui/' exists before writing headers
    os.makedirs(WEB_OUT_DIR, exist_ok=True)
    flash_stamp = get_serial_flash_stamp()
    fw_identity = read_firmware_identity()
    safeboot_identity = read_safeboot_identity()
    print(f"[build_web_ui.py] Firmware identity from config.h: v{fw_identity['@@FW_VERSION@@']} build {fw_identity['@@FW_BUILD@@']}")
    print(f"[build_web_ui.py] Safe Boot identity from config.h:  v{safeboot_identity['@@FW_VERSION@@']} build {safeboot_identity['@@FW_BUILD@@']}")

    for entry in sorted(os.listdir(WEB_SRC_DIR)):
        src_path = os.path.join(WEB_SRC_DIR, entry)
        if not os.path.isfile(src_path) or entry.startswith("."):
            continue

        # Map filename -> header name (lowercase) and PROGMEM symbol (UPPERCASE)
        base_id = sanitize_identifier(entry)
        header_filename = f"{base_id.lower()}.h"
        macro_var_name = base_id.upper()
        out_path = os.path.join(WEB_OUT_DIR, header_filename)

        with open(src_path, "r", encoding="utf-8") as src_file:
            raw_content = src_file.read()

        # safeboot.html shows the SAFE BOOT firmware's own version; every other asset shows the main app's.
        identity = safeboot_identity if entry == "safeboot.html" else fw_identity
        for token, value in identity.items():
            raw_content = raw_content.replace(token, value)

        if macro_var_name == "MAIN_JS":
            raw_content = "\n".join(
                line.strip()
                for line in raw_content.splitlines()
                if line.strip() and not line.strip().startswith("//")
            )

        # Construct C++ header with #pragma once and PROGMEM raw string literal
        header_lines = [
            "// Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).",
            "#pragma once",
            "#include <Arduino.h>",
            "",
            "// =============================================================================",
            f"// AUTO-GENERATED FILE — DO NOT EDIT MANUALLY",
            f"// Source asset : web_src/{entry}",
            f"// Generated by : build_web_ui.py -> src/web_ui/{header_filename}",
            f"// C++ Symbol   : {macro_var_name}",
            "// =============================================================================",
        ]

        if macro_var_name == "INDEX_HTML":
            header_lines.append(f'#define SERIAL_FLASH_STAMP "{flash_stamp}"')
            header_lines.append("")

        # Gzip-compress the asset (mtime=0 -> deterministic output, so unchanged assets are not rewritten).
        # The firmware serves these bytes with "Content-Encoding: gzip" (all browsers support it).
        gz_bytes = gzip.compress(raw_content.encode("utf-8"), compresslevel=9, mtime=0)
        header_lines.append(f"// Raw size: {len(raw_content.encode('utf-8'))} bytes -> gzip: {len(gz_bytes)} bytes")
        header_lines.append(f"const size_t {macro_var_name}_GZ_LEN = {len(gz_bytes)};")
        header_lines.append(f"const uint8_t {macro_var_name}_GZ[] PROGMEM = {{")
        for i in range(0, len(gz_bytes), 24):
            header_lines.append("  " + ",".join(f"0x{b:02x}" for b in gz_bytes[i:i + 24]) + ",")
        header_lines.append("};")
        header_lines.append("")

        new_header_text = "\n".join(header_lines)

        # Write only if changed so incremental SCons builds stay fast
        write_needed = True
        if os.path.exists(out_path):
            with open(out_path, "r", encoding="utf-8") as existing_file:
                if existing_file.read() == new_header_text:
                    write_needed = False

        if write_needed:
            with open(out_path, "w", encoding="utf-8") as out_file:
                out_file.write(new_header_text)
            print(f"[build_web_ui.py] Generated src/web_ui/{header_filename} -> {macro_var_name}_GZ ({len(raw_content)} -> {len(gz_bytes)} bytes gzip)")
        else:
            print(f"[build_web_ui.py] Up-to-date: src/web_ui/{header_filename} ({macro_var_name})")


# Execute immediately during PlatformIO pre-build phase
generate_web_headers()
