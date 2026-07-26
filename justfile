set positional-arguments := true

set dotenv-load := true

# Build parameters, overridable via environment variables
export APP := env_var_or_default("APP", "project/app")
export BOARD := env_var_or_default("BOARD", "nrf9151_connectkit/nrf9151/ns")
export BUILD_TYPE := env_var_or_default("BUILD_TYPE", "Release")
export SYSBUILD := env_var_or_default("SYSBUILD", "true")
export WEST_RUNNER := env_var_or_default("WEST_RUNNER", "jlink")
export WEST_RUNNER_ARGS := env_var_or_default("WEST_RUNNER_ARGS", "")
export BUILD_DIR := env_var_or_default("BUILD_DIR", "build/" + BOARD)
# Explicit toolchain selection; unset it breaks FindZephyr-sdk.cmake under CMake >= 4.4
export ZEPHYR_TOOLCHAIN_VARIANT := env_var_or_default("ZEPHYR_TOOLCHAIN_VARIANT", "zephyr")

[private]
@default:
    just --list

# Build the project. Set BUILD_TYPE=Debug for a debug build (defaults to Release)
build *args: west-setup
    @[ -e {{APP}}/secrets.conf ] || (echo "ERROR: {{APP}}/secrets.conf not found — copy secrets.conf.example next to it and fill in the credentials" && exit 1)
    just west build {{APP}} "$@"

# Open the menuconfig tool
menuconfig:
    just build -t menuconfig

# Clean all build directories
clean:
    rm -rf .cache build compile_commands.json

# Full-chip erase is required when slot0_ns may hold a previous OTA image:
# merged.hex doesn't fully cover slot0_ns, so a sector-only erase would leave a
# corrupt image and MCUboot reverts to slot1_ns.

# Flash merged.hex with a full chip erase
flash:
    pyocd load --erase chip --target nrf91 --frequency 4000000 build/{{BOARD}}/merged.hex

# Reset the target
reset:
    pyocd reset --target nrf91

# MQTT test clients. Credentials and device IMEI come from .env (dotenv-load).

# Subscribe to every topic on the broker
mqtt-sub:
    mqtt sub -s -u "${MQTT_CLI_USER:?set MQTT_CLI_USER in .env}" -pw "$MQTT_CLI_PASSWORD" -t '#' -T

# Ask the device for a battery report
mqtt-bat:
    mqtt pub -s -u "${MQTT_CLI_USER:?set MQTT_CLI_USER in .env}" -pw "$MQTT_CLI_PASSWORD" -t "${DEVICE_IMEI:?set DEVICE_IMEI in .env}/cmd/bat" -m '{"cmd":"bat"}'

# Trigger a FOTA update and confirm the device acknowledged it within 5s
mqtt-fota-update:
    #!/usr/bin/env bash
    set -uo pipefail
    IMEI="${DEVICE_IMEI:?set DEVICE_IMEI in .env}"
    OUT=$(mktemp)
    SUB_PID=""
    trap 'if [ -n "$SUB_PID" ]; then kill "$SUB_PID" 2>/dev/null; fi; rm -f "$OUT"' EXIT
    # Listen for the device's "updating" acknowledgement first
    mqtt sub -s -u "$MQTT_CLI_USER" -pw "$MQTT_CLI_PASSWORD" -t "$IMEI/fota" >"$OUT" 2>/dev/null &
    SUB_PID=$!
    sleep 2  # let the subscription establish
    mqtt pub -s -u "$MQTT_CLI_USER" -pw "$MQTT_CLI_PASSWORD" -t "$IMEI/cmd/fota" -m '{"cmd":"fota_update"}' >/dev/null 2>&1
    for _ in $(seq 1 50); do
        if grep -q updating "$OUT"; then
            echo "Fota update started"
            exit 0
        fi
        sleep 0.1
    done
    echo "Fota update failed to start"
    exit 1


# Run west from the virtual environment
west *args:
    #!/bin/sh
    [ -e .venv/bin/activate ] || (echo "venv not found, run just init" && exit 1)
    . .venv/bin/activate && west "$@"



# Cut a Memfault release. Use --patch / --minor / --major to pick the bump.
release *args:
    .venv/bin/python3 scripts/release.py "$@"

# Initialize the zephyr workspace
init:
    #!/bin/sh
    set -ex
    # If already in a venv, will not automatically create one
    if [ -z "$VIRTUAL_ENV" ]; then
        python -m venv --system-site-packages .venv
        . .venv/bin/activate
        pip install --upgrade pip
        pip install west jsonschema
    fi

    [ -e .west ] || west init -l project
    west update

    pip install $(west packages pip)

    west config build.pristine auto
    west config build.guess-dir runners


# Stop the Onomondo traffic capture service
onomondo-stop:
    sudo systemctl stop onomondo-capture.service

# Restart the Onomondo traffic capture service
onomondo-restart:
    sudo systemctl restart onomondo-capture.service

# Show the Onomondo capture service status
onomondo-status:
    systemctl status onomondo-capture.service

# Follow the Onomondo capture service log
onomondo-log:
    journalctl -u onomondo-capture.service -f

# Stop the MQTT traffic logging service
mqtt-stop:
    sudo systemctl stop mqtt-capture.service

# Restart the MQTT traffic logging service
mqtt-restart:
    sudo systemctl restart mqtt-capture.service

# Show the MQTT logging service status
mqtt-status:
    systemctl status mqtt-capture.service

# Follow the MQTT logging service log
mqtt-log:
    journalctl -u mqtt-capture.service -f

[private]
west-setup:
    just west config build.board {{BOARD}}
    just west config build.sysbuild {{SYSBUILD}}
    just west config build.dir-fmt "{{BUILD_DIR}}"
    just west config build.cmake-args -- " \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_BUILD_TYPE={{BUILD_TYPE}} \
        -DNO_BUILD_TYPE_WARNING=ON \
        -DEXTRA_CONF_FILE=secrets.conf \
    "

