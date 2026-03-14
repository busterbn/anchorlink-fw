set positional-arguments := true

set dotenv-load := true

# Set build parameters here



export APP := env_var_or_default("APP", "project/app")
export BOARD := env_var_or_default("BOARD", "C7/nrf5340/cpuapp")
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

# Flash from within the docker image
flash *args:
    just west flash -d {{BUILD_DIR}} -r {{ WEST_RUNNER }} {{ WEST_RUNNER_ARGS }} "$@"

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

apply_patches:
    # ./patches/apply_patches.sh
    cp patches/common.options deps/nrf/subsys/net/lib/softap_wifi_provision/proto/common.options

provision *args:
    .venv/bin/python3 scripts/cred_import.py "$@"

add_to_jig version:
    cp ./build/C3/nrf5340/cpuapp/app/zephyr/zephyr.signed.hex ../cleverplant-jig/app/firmware/{{version}}.zephyr.signed.hex
    # cp ./build/C3/nrf5340/cpuapp/app/zephyr/zephyr.signed.bin ../cleverplant-jig/fota/{{version}}.zephyr.signed.bin
    # cp ./build/C3/nrf5340/cpuapp/mcuboot/zephyr/zephyr.hex ../cleverplant-jig/app/mcuboot/mcuboot.hex

# Initialize the zephyr workspace
init:
    #!/bin/sh
    set -ex
    # If already in a venv, will not automatically create one
    if [ -z "$VIRTUAL_ENV" ]; then
        python -m venv --system-site-packages .venv
        . .venv/bin/activate
        pip install west
    fi

    [ -e .west ] || west init -l project
    west update

    pip install $(west packages pip)

    west config build.pristine auto
    west config build.guess-dir runners
    just apply_patches

ci-build:
	mkdir -p build
	export ACCEPT_JLINK_LICENSE=1
	docker build --build-arg ACCEPT_JLINK_LICENSE=1 -t cleverplant-builder .
	docker run --privileged -v $(shell pwd)/build:/workdir/build cleverplant-builder "just build"

# Pushes the signed firmware to my update server on my pc
push-fota-to-pc:
    scp build/C3/nrf5340/cpuapp/app/zephyr/zephyr.signed.bin bn@pc:~/repo/fota-server/files/zephyr.signed.bin
    scp project/app/VERSION bn@pc:~/repo/fota-server/files/VERSION

prov serial debug:
    .venv/bin/python scripts/serial_prog.py write {{serial}} {{if debug == "true" { "1" } else { "0" }}}



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

# Compute the rev and version using either CI_COMMIT_TAG which is set by GitLab in CI, or `git describe` for local builds
# Rev: the full version string with possible commit or -dirty suffix. Example: v0.4.0-rc1-dirty or v1.0.0
[private]
generate-version:
    #!/bin/bash
    set -e
    rev="${FW_REV:-${CI_COMMIT_TAG:-}}";
    [ "$rev" ] || rev=$(git describe --dirty 2> /dev/null)
    rev=${rev:-v0.0.0}
    rev=${rev#v}
    [ "$BUILD_TYPE" = "Release" ] || rev="$rev-dev"
    echo "Version determined: $rev" >&2
    version_file={{APP}}/VERSION
    if ! [ -e "$version_file" ] && ! [ -e "{{APP}}/.VERSION" ]; then
        echo "$version_file does not exist. Not updating" >&2
        exit 0
    fi
    echo "Updating version file $version_file:" >&2
    version=${rev%%-*}
    version_suffix=${rev#*-}
    version_file_contents=$(cat <<EOF
    VERSION_MAJOR = $(echo $version | cut -d. -f1)
    VERSION_MINOR = $(echo $version | cut -d. -f2)
    PATCHLEVEL = $(echo $version | cut -d. -f3)
    VERSION_TWEAK = 0
    EXTRAVERSION = $(echo $version_suffix | tr -d [\\-+_])
    EOF
    )
    [[ "$version_file_contents" != $(cat "$version_file") ]] && echo "$version_file_contents" | tee "$version_file" || exit 0

