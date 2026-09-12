#!/usr/bin/env python3
"""Format/check the app entry point, source tree, and host tests."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--check", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
sdk = Path(os.environ.get("UFBT_HOME", str(Path.home() / ".ufbt")))
toolchain = Path(os.environ.get("FBT_TOOLCHAIN_PATH", str(sdk)))
# Prefer the SDK formatter so local checks use the same formatting rules as firmware builds
binaries = sorted(toolchain.glob("toolchain/*/bin/clang-format*"))
formatter = str(binaries[0]) if binaries else shutil.which("clang-format")
if not formatter:
    sys.exit("clang-format missing. Install the official SDK with ufbt update first.")
# Include the entry point and maintained C sources while leaving generated build output alone
sources = [root / "missile_cmd.c"]
for folder in ("src", "tests", "tools"):
    sources.extend(p for p in (root / folder).rglob("*") if p.suffix in (".c", ".h"))
# Checking reports differences without writing; normal mode formats the collected files in place
flags = ["--dry-run", "--Werror"] if args.check else ["-i"]
subprocess.run([formatter, "--style=file", *flags, *map(str, sorted(sources))], check=True)
