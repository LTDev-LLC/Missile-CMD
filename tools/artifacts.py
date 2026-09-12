"""Shared validation and deterministic packaging for builds, installs, and releases."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import zipfile

from build_config import HELP_FILE, app_version, data_directory, help_asset, help_target, validate_version
from pack_fap import LIMIT, parse

FAP_TARGET = '/ext/apps/Games/missile_cmd.fap'
FIRMWARES = ('official', 'unleashed', 'momentum')


def file_sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify_api(data, api):
    _, _, bodies, names = parse(data)
    magic, schema, encoded_api, hardware = struct.unpack_from('<IIIH', bodies[names.index('.fapmeta')])
    major, minor = map(int, api.split('.'))
    if (magic, schema, encoded_api, hardware) != (0x52474448, 1, major << 16 | minor, 7):
        raise ValueError('Built FAP does not match the selected f7 SDK API ' + api)


def artifact_files(root, firmware, version=None):
    version = validate_version(version) if version else app_version(root)
    return [(root / 'dist' / f'missile_cmd-{firmware}.fap', FAP_TARGET),
            (help_asset(root, version), help_target(version))]


def bundle_path(root, firmware):
    return root / 'dist' / f'missile_cmd-{firmware}.zip'


def make_bundle(root, firmware):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', compression=zipfile.ZIP_STORED) as archive:
        for path, target in sorted(artifact_files(root, firmware), key=lambda item: item[1]):
            info = zipfile.ZipInfo(target.removeprefix('/ext/'), (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, path.read_bytes())
    bundle_path(root, firmware).write_bytes(buffer.getvalue())


def verify_artifact(root, firmware, sdk=None, version=None):
    if firmware not in FIRMWARES:
        raise ValueError('Unknown firmware')
    report = json.loads((root / 'build_metadata' / firmware / 'sdk.json').read_text())
    files = artifact_files(root, firmware, report['app_version'])
    data = files[0][0].read_bytes()
    if report['firmware'] != firmware or report['sha256'] != hashlib.sha256(data).hexdigest() or \
            (sdk and any(report[key] != sdk[key] for key in ('firmware', 'version', 'api_version'))):
        raise ValueError(f'Artifact or SDK changed; run make build FIRMWARE={firmware}')
    if len(data) > LIMIT:
        raise ValueError('Packed FAP exceeds its size limit')
    verify_api(data, report['api_version'])
    if version and (report['app_version'] != version or version.encode() not in data):
        raise ValueError('Release version differs from the built app')
    if (root / 'VERSION').exists() and report['app_version'] != app_version(root):
        raise ValueError('App version changed; rebuild before installing')
    if report['help_file'] != HELP_FILE or report['help_sha256'] != file_sha256(files[1][0]):
        raise ValueError('Help artifact changed; rebuild the app and help together')
    return files


def verify_bundle(root, firmware):
    report = json.loads((root / 'build_metadata' / firmware / 'sdk.json').read_text())
    path = bundle_path(root, firmware)
    if report['bundle_sha256'] != file_sha256(path):
        raise ValueError('Bundle changed after verification')
    with zipfile.ZipFile(path) as archive:
        expected = {target.removeprefix('/ext/'): path.read_bytes()
                    for path, target in artifact_files(root, firmware, report['app_version'])}
        if sorted(archive.namelist()) != sorted(expected):
            raise ValueError('Unexpected bundle contents')
        for name, data in expected.items():
            if archive.read(name) != data:
                raise ValueError('Bundle contains stale app or help data')


def prepare_release(root, version):
    for firmware in FIRMWARES:
        verify_artifact(root, firmware, version=version)
        verify_bundle(root, firmware)
    shutil.copy2(root / 'dist/missile_cmd-official.fap', root / 'dist/missile_cmd.fap')
    # Publish the actual build metadata with the binaries, never the current lock.
    manifest = {'schema': 2, 'version': validate_version(version),
                'data_directory': data_directory(version), 'builds': []}
    for firmware in FIRMWARES:
        report = json.loads((root / 'build_metadata' / firmware / 'sdk.json').read_text())
        manifest['builds'].append({
            'firmware': firmware, 'firmware_version': report['version'],
            'api_version': report['api_version'],
            'files': [{'name': path.name, 'target': target,
                       'sha256': file_sha256(path)}
                      for path, target in artifact_files(root, firmware, version)],
        })
    (root / 'dist/install-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    prepare_release(args.root, args.version)
