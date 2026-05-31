set positional-arguments := true

set dotenv-load := true

# Set build parameters here



export APP := env_var_or_default("APP", "project/app")
export BOARD := env_var_or_default("BOARD", "nrf9151_connectkit/nrf9151/ns")
# export BUILD_TYPE := env_var_or_default("BUILD_TYPE", "MinSizeRel") # Set to 'Release' for release builds
# export BUILD_TYPE := env_var_or_default("BUILD_TYPE", "Debug") # Set to 'Release' for release builds
export BUILD_TYPE := env_var_or_default("BUILD_TYPE", "Release") # Set to 'Release' for release builds
export SYSBUILD := env_var_or_default("SYSBUILD", "true")
export WEST_RUNNER := env_var_or_default("WEST_RUNNER", "jlink")
export WEST_RUNNER_ARGS := env_var_or_default("WEST_RUNNER_ARGS", "")
export BUILD_DIR := env_var_or_default("BUILD_DIR", "build/" + BOARD)

DOCKER_IMG_NAME := "cleverplant-builder"

[private]
@default:
    just --list

# Build the project. Set the BUILD_TYPE variable to `Debug` or `Release`. Defaults to `Debug`
build *args: west-setup
    just west build {{APP}} "$@"

release-build *args: west-setup
    # echo "Building release"
    just west build {{APP}} "$@" -- -DEXTRA_CONF_FILE=release.conf

# Open the menuconfig tool
menuconfig:
    just build -t menuconfig

# Clean all build directories
clean:
    rm -rf .cache build compile_commands.json

# # Flash from within the docker image
# flash *args:
#     just west flash -d {{BUILD_DIR}} -r {{ WEST_RUNNER }} {{ WEST_RUNNER_ARGS }} "$@"

# Flash merged.hex with a full chip erase. Required when slot0_ns may hold a
# previous OTA image: merged.hex doesn't fully cover slot0_ns, so a sector-only
# erase leaves a corrupt image and MCUboot reverts to slot1_ns.
flash:
    pyocd load --erase chip --target nrf91 --frequency 4000000 build/{{BOARD}}/merged.hex

reset:
    pyocd reset --target nrf91

mqtt-sub:
    mqtt sub -s -u macbook -pw '***REMOVED***' -t '#' -T

mqtt-update:

mqtt-bat:
    mqtt pub -s -u macbook -pw '***REMOVED***' -t '359404230194475/cmd/bat' -m 'bat'

mqtt-fota-update:
    mqtt pub -s -u macbook -pw '***REMOVED***' -t '359404230194475/cmd/foo' -m 'fota_update'

# Run a debugserver and RTT logging
run:
    just run-{{WEST_RUNNER}}

# Run west from the virtual environment
west *args:
    #!/bin/sh
    [ -e .venv/bin/activate ] || (echo "venv not found, run just init" && exit 1)
    . .venv/bin/activate && west "$@"

debug *args:
    just west debug -d {{BUILD_DIR}} -r {{ WEST_RUNNER }} {{ WEST_RUNNER_ARGS }} "$@"

rtt:
    just west rtt -r {{WEST_RUNNER}} {{WEST_RUNNER_ARGS}}

provision *args:
    .venv/bin/python3 scripts/cred_import.py "$@"

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


[private]
west-setup:
    just west config build.board {{BOARD}}
    just west config build.sysbuild {{SYSBUILD}}
    just west config build.dir-fmt "{{BUILD_DIR}}"
    just west config build.cmake-args -- " \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_BUILD_TYPE={{BUILD_TYPE}} \
        -DNO_BUILD_TYPE_WARNING=ON \
    "

