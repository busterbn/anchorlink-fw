#!/usr/bin/env python3
"""Bump firmware version, build, and ship a release to Memfault."""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PRJ_CONF = REPO / "project/app/prj.conf"
BUILD_DIR = REPO / "build/nrf9151_connectkit/nrf9151/ns/app/zephyr"
PAYLOAD = BUILD_DIR / "zephyr.signed.bin"
SYMBOLS = BUILD_DIR / "zephyr.elf"

HARDWARE_VERSION = "nrf9151_connectkit"
SOFTWARE_TYPE = "app"
COHORT = "default"

VERSION_RE = re.compile(r'CONFIG_MEMFAULT_NCS_FW_VERSION="(\d+)\.(\d+)\.(\d+)"')


def read_version():
    text = PRJ_CONF.read_text()
    m = VERSION_RE.search(text)
    if not m:
        sys.exit(f"could not find CONFIG_MEMFAULT_NCS_FW_VERSION=\"X.Y.Z\" in {PRJ_CONF}")
    return tuple(int(x) for x in m.groups())


def write_version(version):
    text = PRJ_CONF.read_text()
    new_text = VERSION_RE.sub(f'CONFIG_MEMFAULT_NCS_FW_VERSION="{version}"', text)
    PRJ_CONF.write_text(new_text)


def run(*cmd):
    print(f"\n>>> {' '.join(cmd)}\n", flush=True)
    subprocess.run(cmd, check=True)


def main():
    p = argparse.ArgumentParser(
        prog="just release",
        description="Bump CONFIG_MEMFAULT_NCS_FW_VERSION, build, upload, and activate.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  just release --patch    # X.Y.Z   -> X.Y.(Z+1)\n"
            "  just release --minor    # X.Y.Z   -> X.(Y+1).0\n"
            "  just release --major    # X.Y.Z   -> (X+1).0.0\n"
        ),
    )
    g = p.add_mutually_exclusive_group()
    g.add_argument("--patch", action="store_true", help="bump patch (X.Y.Z+1)")
    g.add_argument("--minor", action="store_true", help="bump minor (X.Y+1.0)")
    g.add_argument("--major", action="store_true", help="bump major (X+1.0.0)")
    args = p.parse_args()

    if not (args.patch or args.minor or args.major):
        p.print_help()
        major, minor, patch = read_version()
        print(f"\nCurrent version: {major}.{minor}.{patch}")
        sys.exit(0)

    org = os.environ.get("MEMFAULT_ORG")
    project = os.environ.get("MEMFAULT_PROJECT")
    org_token = os.environ.get("MEMFAULT_ORG_TOKEN")
    if not (org and project and org_token):
        sys.exit("MEMFAULT_ORG, MEMFAULT_PROJECT and MEMFAULT_ORG_TOKEN must be set")

    if not shutil.which("memfault"):
        sys.exit("memfault CLI not found in PATH (try: pipx install memfault-cli)")

    major, minor, patch = read_version()
    if args.major:
        major, minor, patch = major + 1, 0, 0
    elif args.minor:
        minor, patch = minor + 1, 0
    else:
        patch += 1
    new_version = f"{major}.{minor}.{patch}"

    print(f"Releasing {new_version}")
    original_conf = PRJ_CONF.read_text()
    write_version(new_version)

    run("git", "add", "-A")
    run("git", "commit", "-m", f"Release version {new_version}")
    run("git", "tag", "-a", f"v{new_version}", "-m", f"Release v{new_version}")
    run("git", "push")
    run("git", "push", "--tags")

    try:
        run("just", "build", "-p")

        if not PAYLOAD.exists() or not SYMBOLS.exists():
            sys.exit(f"missing build artifacts in {BUILD_DIR}")

        run("memfault", "--org", org, "--project", project, "--org-token", org_token,
            "upload-ota-payload",
            "--hardware-version", HARDWARE_VERSION,
            "--software-type", SOFTWARE_TYPE,
            "--software-version", new_version,
            str(PAYLOAD))

        run("memfault", "--org", org, "--project", project, "--org-token", org_token,
            "upload-mcu-symbols",
            "--software-type", SOFTWARE_TYPE,
            "--software-version", new_version,
            str(SYMBOLS))

        run("memfault", "--org", org, "--project", project, "--org-token", org_token,
            "deploy-release",
            "--release-version", new_version,
            "--cohort", COHORT)
    except (subprocess.CalledProcessError, SystemExit):
        print(f"\nrelease failed — reverting {PRJ_CONF.name}", file=sys.stderr)
        PRJ_CONF.write_text(original_conf)
        raise

    print(f"\n✓ Release {new_version} active for cohort '{COHORT}'")
    print(f"  Remember to commit the version bump in {PRJ_CONF.relative_to(REPO)}")


if __name__ == "__main__":
    main()
