#!/usr/bin/env python3
"""Install the verified FAP for the selected firmware through its SDK's USB uploader."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

from firmware import ROOT, REPOSITORIES, environment, sdk_info
from artifacts import verify_artifact


def upload_command(firmware, env):
    files = verify_artifact(ROOT, firmware, sdk=sdk_info(firmware, env))
    sdk = Path(env["UFBT_HOME"]) / "current"
    uploader = sdk / "scripts/runfap.py"
    toolchain = Path(env["FBT_TOOLCHAIN_PATH"])
    runtimes = sorted(p for p in toolchain.glob("toolchain/*/bin/python3*")
                      if re.fullmatch(r"python3(?:\.\d+)?(?:\.exe)?", p.name))
    if not uploader.is_file() or not runtimes:
        raise ValueError(f"SDK runtime missing; run make build FIRMWARE={firmware}")
    return [str(runtimes[0]), str(Path(__file__).with_name("upload.py")), str(uploader),
            "-p", env.get("FLIP_PORT", "auto"),
            "-s", *[str(path) for path, _ in files], "-t", *[target for _, target in files]]


def install(firmware, env):
    subprocess.run(upload_command(firmware, env), cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", choices=REPOSITORIES, default=os.environ.get("FIRMWARE", "official"))
    args = parser.parse_args()
    try:
        install(args.firmware, environment(args.firmware))
    except (OSError, ValueError, KeyError, StopIteration, subprocess.CalledProcessError) as error:
        sys.exit(f"Install failed: {error}")
