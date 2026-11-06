# Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
#
# Custom PlatformIO target for [env:flash_full]: erase the ENTIRE ESP32 flash chip, build
# BOTH firmwares fresh (the safe-boot/recovery image and the main diagnostic app), then write
# bootloader + partition table + boot_app0 + the safe-boot firmware (factory slot) + the main
# app (ota_0 slot) all in one command.
#
# This is for a brand-new/blank board, or to force a totally clean re-flash. It is NOT the
# same as a normal `pio run -e esp32_hd -t upload`:
#   - it fully erases the chip first (esptool erase_flash) - this WIPES NVS too, so all saved
#     settings (Wi-Fi SSID/password, CAN bitrate, etc.) are reset to firmware defaults.
#   - it ALSO flashes the safe-boot/recovery firmware into the factory slot (0x10000), which a
#     normal `-t upload` of esp32_hd never touches.
#
# Usage:
#     pio run -e flash_full -t fullflash
#     pio run -e flash_full -t fullflash --upload-port /dev/ttyUSB0   (if autodetection fails)
#
# What gets written, and where (see partitions_4mb.csv for the layout):
#     0x1000    bootloader.bin    (from the esp32_hd build - shared by both firmwares)
#     0x8000    partitions.bin    (partition table, from partitions_4mb.csv)
#     0xe000    boot_app0.bin     (selects the ota_0 slot as bootable on first boot)
#     0x140000  firmware.bin      (main diagnostic app, esp32_hd build -> ota_0 partition)
#     0x10000   firmware.bin      (safe-boot/recovery app, safeboot_hd build -> factory partition)
#
# If this custom target ever fails to run on your PlatformIO Core version, the same result can
# always be reproduced by hand - see the "MANUAL EQUIVALENT" block at the bottom of this file.

Import("env")
import os
import subprocess
import sys


def full_flash(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")
    port = env.subst("$UPLOAD_PORT") or ""
    speed = env.subst("$UPLOAD_SPEED") or "921600"
    python = sys.executable
    port_args = ["--port", port] if port else []

    def run(cmd):
        print("[flash_full] $ " + " ".join(str(c) for c in cmd))
        subprocess.check_call(cmd, cwd=project_dir)

    # 1) Build BOTH firmware images fresh (each env's own pre-build hook, include paths and
    #    source filters apply normally - this is a plain `pio run` per environment).
    run([python, "-m", "platformio", "run", "-e", "esp32_hd"])
    run([python, "-m", "platformio", "run", "-e", "safeboot_hd"])

    build_main = os.path.join(project_dir, ".pio", "build", "esp32_hd")
    build_safe = os.path.join(project_dir, ".pio", "build", "safeboot_hd")

    # 2) Fully erase the chip. This wipes NVS (saved settings) too - that is intentional for a
    #    clean/first flash, not a side effect to work around.
    run([python, "-m", "esptool", "--chip", "esp32"] + port_args + ["erase_flash"])

    # 3) Write everything: bootloader + partition table + boot_app0 (shared layout, built once
    #    from the esp32_hd env since both environments use the same board/partitions_4mb.csv),
    #    the main app at ota_0, and the safe-boot firmware at the factory slot. The five
    #    regions never overlap, so write order does not matter.
    run([python, "-m", "esptool", "--chip", "esp32"] + port_args + ["--baud", speed, "write_flash",
         "0x1000",   os.path.join(build_main, "bootloader.bin"),
         "0x8000",   os.path.join(build_main, "partitions.bin"),
         "0xe000",   os.path.join(build_main, "boot_app0.bin"),
         "0x140000", os.path.join(build_main, "firmware.bin"),
         "0x10000",  os.path.join(build_safe, "firmware.bin")])

    print("[flash_full] Done: chip erased; bootloader + partition table + boot_app0 written; "
          "safe-boot firmware in the factory slot (0x10000); main app in ota_0 (0x140000).")


env.AddCustomTarget(
    name="fullflash",
    dependencies=None,
    actions=[full_flash],
    title="Full erase + flash everything",
    description="Erase the whole chip, build both firmwares, and write bootloader+partitions+safeboot+app",
)

# -----------------------------------------------------------------------------------------
# MANUAL EQUIVALENT (run these yourself if the custom target above does not work for you):
#
#   pio run -e esp32_hd
#   pio run -e safeboot_hd
#   python -m esptool --chip esp32 --port <PORT> erase_flash
#   python -m esptool --chip esp32 --port <PORT> --baud 921600 write_flash ^
#       0x1000   .pio/build/esp32_hd/bootloader.bin ^
#       0x8000   .pio/build/esp32_hd/partitions.bin ^
#       0xe000   .pio/build/esp32_hd/boot_app0.bin ^
#       0x140000 .pio/build/esp32_hd/firmware.bin ^
#       0x10000  .pio/build/safeboot_hd/firmware.bin
#
# (use \ instead of ^ as the line-continuation character on macOS/Linux)
# -----------------------------------------------------------------------------------------
