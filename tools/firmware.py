#!/usr/bin/env python3
"""Build against isolated Official, Unleashed, and Momentum release SDKs."""
import argparse
import csv
import fcntl
import hashlib
import json
import os
import platform
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile
from urllib.parse import quote
from urllib.request import Request, urlopen

from pack_fap import parse
from artifacts import file_sha256, verify_api, make_bundle, bundle_path
from help_assets import generate as generate_help
from build_config import HELP_FILE, app_version, help_asset

ROOT = Path(__file__).resolve().parents[1]
REPOSITORIES = {
    "official": "flipperdevices/flipperzero-firmware",
    "unleashed": "DarkFlippers/unleashed-firmware",
    "momentum": "Next-Flip/Momentum-Firmware",
}
VERSION_PATTERNS = {
    "official": r"\d+\.\d+\.\d+",
    "unleashed": r"unlshd-\d+",
    "momentum": r"mntm-\d+",
}


def build_lock():
    path = Path(os.environ.get("SDK_LOCK_FILE", ROOT / "build-lock.json"))
    return json.loads(path.read_text())



def download_sdk(firmware, version, url, env, expected=None):
    cache = Path(env["UFBT_HOME"]) / "downloads"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / ((expected or hashlib.sha256(url.encode()).hexdigest()) + ".zip")
    # Reuse archives downloaded while preparing the committed lock, after verifying bytes.
    local = ROOT / "build" / "sdk-archives" / (firmware + ".zip")
    if not archive.exists() and expected and local.exists() and file_sha256(local) == expected:
        shutil.copyfile(local, archive)
    if not archive.exists():
        partial = archive.with_suffix(".part")
        try:
            with urlopen(url, timeout=60) as response, partial.open("wb") as stream:
                shutil.copyfileobj(response, stream)
            digest = file_sha256(partial)
            if expected and digest != expected:
                raise ValueError(f"SDK archive checksum mismatch for {firmware} {version}")
            partial.replace(archive)
        finally:
            partial.unlink(missing_ok=True)
    digest = file_sha256(archive)
    if expected and digest != expected:
        raise ValueError(f"Cached SDK archive checksum mismatch: {archive}")
    return archive, {"version": version, "url": url, "sha256": digest}


def verify_sdk_tree(archive, sdk):
    # Compare all publisher-provided files, not merely a mutable cache receipt.
    with zipfile.ZipFile(archive) as bundle:
        for member in bundle.infolist():
            if member.is_dir():
                continue
            relative = Path(member.filename)
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError("Unsafe SDK archive member")
            installed = sdk / relative
            if not installed.is_file():
                return False
            with bundle.open(member) as source:
                if hashlib.file_digest(source, "sha256").hexdigest() != file_sha256(installed):
                    return False
    return True


def verify_bootstrap(env):
    result = subprocess.run(["ufbt", "status", "--json"], env=env, text=True, capture_output=True)
    status = json.loads(result.stdout)
    # uFBT reports its package version even when a fresh SDK home makes status exit 1.
    if result.returncode and not (result.returncode == 1 and status.get("error") == "SDK is not deployed"):
        result.check_returncode()
    version = status["ufbt_version"]
    if version != build_lock()["ufbt"]:
        raise ValueError("uFBT version differs from build-lock.json; install requirements-build.txt")
    return version


def toolchain_platform():
    return platform.machine().lower() + '-' + platform.system().lower()


def verify_toolchain_tree(archive, installed):
    digests = {}
    # Hard links refer to earlier members. Reuse their hashes instead of seeking
    # backwards through gzip and decompressing the toolchain again for every link.
    with tarfile.open(archive, 'r|gz') as bundle:
        for member in bundle:
            relative = Path(member.name)
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError('Unsafe toolchain archive member')
            path = installed.joinpath(*relative.parts[1:])
            if member.issym():
                if not path.is_symlink() or os.readlink(path) != member.linkname:
                    return False
            elif member.isfile() or member.islnk():
                if path.is_symlink() or not path.is_file():
                    return False
                if member.islnk():
                    if member.linkname not in digests:
                        raise ValueError('Toolchain hard link has no preceding file target')
                    digest = digests[member.linkname]
                else:
                    with bundle.extractfile(member) as source:
                        digest = hashlib.file_digest(source, 'sha256').hexdigest()
                digests[member.name] = digest
                if digest != file_sha256(path):
                    return False
            elif not member.isdir():
                raise ValueError('Unsupported toolchain archive entry')
    return True


def ensure_toolchain(env):
    started = time.monotonic()
    system = toolchain_platform()
    entry = build_lock()['toolchains'][system]
    root = Path(env['FBT_TOOLCHAIN_PATH']) / 'toolchain'
    root.mkdir(parents=True, exist_ok=True)
    archive = root / (entry['sha256'] + '.tar.gz')
    if not archive.exists():
        print(f'Fetching locked {system} toolchain archive', flush=True)
        local = ROOT / 'build/toolchain-archives' / (system + '.tar.gz')
        partial = archive.with_suffix('.part')
        try:
            if local.exists():
                shutil.copyfile(local, partial)
            else:
                with urlopen(entry['url'], timeout=60) as source, partial.open('wb') as out:
                    shutil.copyfileobj(source, out)
            if file_sha256(partial) != entry['sha256']:
                raise ValueError('Toolchain archive checksum mismatch')
            partial.replace(archive)
        finally:
            partial.unlink(missing_ok=True)
    print(f'Checking {system} toolchain archive checksum', flush=True)
    if file_sha256(archive) != entry['sha256']:
        raise ValueError('Cached toolchain archive checksum mismatch')
    installed = root / system
    if not installed.exists():
        print(f'Extracting {system} toolchain', flush=True)
        with tempfile.TemporaryDirectory(dir=root) as temporary:
            with tarfile.open(archive, 'r:gz') as bundle:
                bundle.extractall(temporary, filter='data')
            unpacked, = Path(temporary).iterdir()
            shutil.move(unpacked, installed)
    print(f'Verifying {system} toolchain files', flush=True)
    if not verify_toolchain_tree(archive, installed):
        raise ValueError(f'Toolchain cache differs from its locked archive: {installed}')
    current = root / 'current'
    if current.is_symlink():
        current.unlink()
    elif current.exists():
        raise ValueError(f'Expected a toolchain symlink: {current}')
    current.symlink_to(installed, target_is_directory=True)
    print(f'Toolchain ready in {time.monotonic() - started:.1f}s', flush=True)
    return {'toolchain_platform': system, 'toolchain_archive_sha256': entry['sha256'],
            'toolchain_archive_url': entry['url'], 'python_version': platform.python_version(),
            'compiler_version': subprocess.check_output(
                [str(installed / 'bin/arm-none-eabi-gcc'), '-dumpfullversion', '-dumpversion'],
                text=True).strip()}


def resolve_lock(latest=False):
    resolved = build_lock()
    if latest:
        for firmware in REPOSITORIES:
            version, url = release_sdk(firmware)
            _, resolved["firmwares"][firmware] = download_sdk(firmware, version, url, environment(firmware))
    return resolved


def environment(firmware):
    if firmware not in REPOSITORIES:
        raise ValueError(f"Unknown firmware: {firmware}")
    env = os.environ.copy()
    env["FBT_TOOLCHAIN_VERSION"] = str(build_lock()["toolchain"])
    cache = Path(env.get("SDK_ROOT", Path.home() / ".cache/missile-cmd/ufbt")).expanduser()
    env["UFBT_HOME"] = str((cache / firmware).resolve())
    env["FBT_TOOLCHAIN_PATH"] = str(Path(env.get("FBT_TOOLCHAIN_PATH", Path.home() / ".ufbt")).expanduser().resolve())
    # uFBT loads .env after the process environment, which could silently switch SDKs
    dotenv = ROOT / ".env"
    if dotenv.exists():
        for line in dotenv.read_text().splitlines():
            if "=" not in line or line.lstrip().startswith("#"):
                continue
            key, value = line.strip().split("=", 1)
            if key in ("UFBT_HOME", "FBT_TOOLCHAIN_PATH") and value != env[key]:
                raise ValueError(f".env overrides {key}; remove that override when using FIRMWARE")
    return env


def release_sdk(firmware, version="latest"):
    # Official hosts SDKs in its update index; the forks attach them to GitHub releases
    if firmware == "official":
        with urlopen("https://update.flipperzero.one/firmware/directory.json", timeout=30) as response:
            directory = json.load(response)
        releases = next(c["versions"] for c in directory["channels"] if c["id"] == "release")
        candidates = [r for r in releases if version == "latest" or r["version"] == version]
        if not candidates:
            raise ValueError(f"Official release {version} is absent from the release index")
        release = max(candidates, key=lambda r: r["timestamp"])
        tag = release["version"]
        if not re.fullmatch(VERSION_PATTERNS[firmware], tag):
            raise ValueError(f"Unexpected Official release version: {tag}")
        assets = [f for f in release["files"] if f["target"] == "f7" and f["type"] == "sdk_zip"]
        expected = f"https://update.flipperzero.one/builds/firmware/{tag}/flipper-z-f7-sdk-{tag}.zip"
        if len(assets) != 1 or assets[0]["url"] != expected:
            raise ValueError(f"Official release {tag} has no unique f7 SDK")
        return tag, expected
    repo = REPOSITORIES[firmware]
    endpoint = "latest" if version == "latest" else "tags/" + quote(version, safe="")
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "missile-cmd-build"}
    if token := os.environ.get("GH_TOKEN"):
        headers["Authorization"] = f"Bearer {token}"
    request = Request(f"https://api.github.com/repos/{repo}/releases/{endpoint}", headers=headers)
    with urlopen(request, timeout=30) as response:
        release = json.load(response)
    tag = release["tag_name"]
    if not re.fullmatch(VERSION_PATTERNS[firmware], tag):
        raise ValueError(f"Unexpected {firmware} release tag: {tag}")
    expected = f"flipper-z-f7-sdk-{tag}.zip"
    assets = [a for a in release["assets"] if a["name"] == expected]
    if len(assets) != 1:
        raise ValueError(f"Release {tag} must provide exactly one {expected}")
    url = assets[0]["browser_download_url"]
    if url != f"https://github.com/{repo}/releases/download/{tag}/{expected}":
        raise ValueError("Unexpected SDK download URL")
    return tag, url


def sdk_info(firmware, env):
    sdk = Path(env["UFBT_HOME"]) / "current"
    if not (sdk / "components.json").is_file():
        raise ValueError(f"SDK missing; run make sdk-update FIRMWARE={firmware}")
    meta = json.loads((sdk / "components.json").read_text())["meta"]
    version = meta["version"]
    if meta["hw_target"] != "f7" or not re.fullmatch(VERSION_PATTERNS[firmware], version):
        raise ValueError(f"SDK does not match {firmware}/f7; run make sdk-update FIRMWARE={firmware}")
    symbols = sdk / "sdk_headers/f7_sdk/targets/f7/api_symbols.csv"
    with symbols.open(newline="") as stream:
        api = next(row["name"] for row in csv.DictReader(stream) if row["entry"] == "Version")
    if not re.fullmatch(r"\d+\.\d+", api):
        raise ValueError(f"Invalid SDK API version: {api}")
    return {"firmware": firmware, "version": version, "api_version": api,
            "release_url": f"https://github.com/{REPOSITORIES[firmware]}/releases/tag/{version}"}


def run(command, env):
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def ensure_sdk(firmware, env, update=False):
    locked = build_lock()["firmwares"][firmware]
    version = os.environ.get("SDK_VERSION", locked["version"])
    if version != "latest" and not re.fullmatch(VERSION_PATTERNS[firmware], version):
        raise ValueError(f"Invalid SDK_VERSION for {firmware}: {version}")
    sdk = Path(env["UFBT_HOME"]) / "current"
    receipt = sdk / "missile-sdk.json"
    if version == "latest" and not update and (sdk / "components.json").exists():
        version = sdk_info(firmware, env)["version"]
    if version == locked["version"]:
        selected = locked
    elif receipt.exists() and not update and json.loads(receipt.read_text())["version"] == version:
        selected = json.loads(receipt.read_text())
    else:
        version, url = release_sdk(firmware, version)
        selected = {"version": version, "url": url}
    archive, verified = download_sdk(firmware, selected["version"], selected["url"], env, selected.get("sha256"))
    if update or not verify_sdk_tree(archive, sdk):
        print(f"Restoring verified {firmware} SDK {verified['version']}", flush=True)
        run(["ufbt", "update", "--hw-target", "f7", "--local", str(archive)], env)
    info = sdk_info(firmware, env)
    if info["version"] != verified["version"]:
        raise ValueError("Downloaded SDK version does not match the selected release")
    if not verify_sdk_tree(archive, sdk):
        raise ValueError("Installed SDK differs from its verified archive")
    receipt.write_text(json.dumps(verified, indent=2) + "\n")
    info.update(sdk_archive_sha256=verified["sha256"], sdk_archive_url=verified["url"])
    return info


def artifact_path(firmware):
    return ROOT / "dist" / f"missile_cmd-{firmware}.fap"



def build(firmware, env, info):
    help_data = generate_help(ROOT)
    raw = ROOT / "dist/missile_cmd.fap"
    # SDKs share uFBT's dist path; force a fresh install instead of trusting another target's file
    raw.unlink(missing_ok=True)
    run(["ufbt", "FORCE=1", "STRICT_FAP_IMPORT_CHECK=1"], env)
    verify_api(raw.read_bytes(), info["api_version"])
    metadata = ROOT / "build_metadata" / firmware
    run([sys.executable, "tools/pack_fap.py", "--metadata-dir", str(metadata)], env)
    artifact = artifact_path(firmware)
    shutil.copy2(raw, artifact)
    report = dict(info, app_version=app_version(ROOT),
                  artifact=artifact.name, sha256=hashlib.sha256(artifact.read_bytes()).hexdigest())
    toolchain_version = Path(env["FBT_TOOLCHAIN_PATH"]) / "toolchain/current/VERSION"
    if not toolchain_version.is_file() or toolchain_version.read_text().strip() != str(build_lock()["toolchain"]):
        raise ValueError("Compiler toolchain differs from build-lock.json")
    report["toolchain_version"] = toolchain_version.read_text().strip()
    report["ufbt_version"] = verify_bootstrap(env)
    report.update(help_file=HELP_FILE, help_sha256=hashlib.sha256(help_data).hexdigest(),
                  help_bytes=len(help_data), installed_bytes=artifact.stat().st_size + len(help_data))
    make_bundle(ROOT, firmware)
    report['bundle_sha256'] = file_sha256(bundle_path(ROOT, firmware))
    size_report = metadata / 'size.json'
    if size_report.exists():
        sizes = json.loads(size_report.read_text())
        sizes.update(help_bytes=len(help_data), installed_bytes=report['installed_bytes'])
        size_report.write_text(json.dumps(sizes, indent=2) + '\n')
    (metadata / "sdk.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Built {artifact.relative_to(ROOT)} for {firmware} {info['version']} / API {info['api_version']}")
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(summary, "a") as stream:
            stream.write(f"\n{firmware}: {info['version']}, API {info['api_version']} — `{artifact.name}`\n")


def lint(env, apply=False, check_sources=True):
    task = "format" if apply else "lint"
    excludes = " ".join(f"!{ROOT / path}" for path in ("build", "dist", "build_metadata"))
    run(["ufbt", task, f"ARGS={excludes}"], env)
    if check_sources:
        run([sys.executable, "tools/format_sources.py", *([] if apply else ["--check"])], env)


def reproducibility_check(firmware, env, info):
    hashes, deliveries = [], []
    for _ in range(2):
        run(['ufbt', '-c'], env)
        build(firmware, env, info)
        hashes.append(file_sha256(artifact_path(firmware)))
        deliveries.append({path.name: file_sha256(path) for path in
                           (help_asset(ROOT), bundle_path(ROOT, firmware))})
    if hashes[0] != hashes[1] or deliveries[0] != deliveries[1]:
        raise ValueError(f'{firmware}: two clean packed builds differ')
    report = {'clean_build_sha256': hashes, 'delivery_sha256': deliveries, 'identical': True}
    (ROOT / 'build_metadata' / firmware / 'reproducibility.json').write_text(
        json.dumps(report, indent=2) + '\n')
    print(f'{firmware}: two clean packed builds are identical ({hashes[0]})')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("task", choices=("build", "install", "sdk-update", "sdk-status", "lint", "format", "build-all", "resolve", "sdk-lint", "repro-check"))
    parser.add_argument("--firmware", choices=REPOSITORIES, default=os.environ.get("FIRMWARE", "official"))
    parser.add_argument("--latest", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.task == "resolve":
        data = json.dumps(resolve_lock(args.latest), indent=2) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(data)
        else:
            print(data, end="")
        return
    # Serialize builds and SDK updates because uFBT publishes all targets to the same raw path
    (ROOT / "build").mkdir(exist_ok=True)
    with (ROOT / "build/firmware.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for firmware in (REPOSITORIES if args.task == "build-all" else [args.firmware]):
            env = environment(firmware)
            verify_bootstrap(env)
            info = sdk_info(firmware, env) if args.task == "sdk-status" else \
                ensure_sdk(firmware, env, update=args.task == "sdk-update")
            if args.task in ("sdk-update", "sdk-status"):
                print(json.dumps(info, indent=2))
            elif args.task in ("build", "build-all", "install", "repro-check"):
                info.update(ensure_toolchain(env))
                if args.task == 'repro-check':
                    reproducibility_check(firmware, env, info)
                else:
                    build(firmware, env, info)
                if args.task == "install":
                    from install import install
                    install(firmware, env)
            else:
                ensure_toolchain(env)
                lint(env, apply=args.task == "format", check_sources=args.task != "sdk-lint")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, StopIteration, struct.error, subprocess.CalledProcessError) as error:
        sys.exit(f"Firmware build failed: {error}")
