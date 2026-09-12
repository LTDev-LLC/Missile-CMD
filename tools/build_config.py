"""The VERSION file owns the app identity and every versioned data path."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent


HELP_FILE = 'help.bin'
DATA_ROOT = '/ext/apps_data/missile_cmd'
NUMBER = r'(?:0|[1-9][0-9]*)'
PRERELEASE = r'(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)'
SEMVER = re.compile(rf'{NUMBER}\.{NUMBER}\.{NUMBER}'
                    rf'(?:-{PRERELEASE}(?:\.{PRERELEASE})*)?'
                    r'(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?')


def validate_version(value):
    # A FAT filename is at most 255 characters. SemVer excludes path separators,
    # quotes, shell expansion, whitespace, and traversal components.
    if not isinstance(value, str) or len(value) > 255 or not SEMVER.fullmatch(value):
        raise ValueError('VERSION must contain a valid SemVer of at most 255 characters')
    return value


def app_version(root=ROOT):
    return validate_version((root / 'VERSION').read_text().strip())


def data_directory(version):
    return f'{DATA_ROOT}/{validate_version(version)}'


def help_target(version):
    return f'{data_directory(version)}/{HELP_FILE}'


def help_asset(root=ROOT, version=None):
    return root / 'dist' / validate_version(version or app_version(root)) / HELP_FILE


def generate(root=ROOT):
    version = app_version(root)
    header = ('// Generated from VERSION by tools/build_config.py.\n#pragma once\n'
              f'#define MC_APP_VERSION "{version}"\n'
              f'#define MC_HELP_FILE "{HELP_FILE}"\n'
              f'#define MC_HELP_ASSET_PATH "dist/{version}/{HELP_FILE}"\n')
    path = root / 'build/generated/build_version.h'
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != header:
        path.write_text(header)
    return version
