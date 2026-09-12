#!/usr/bin/env python3
"""Mirror published GitHub release assets into a self-contained Pages installer.

Run with GitHub CLI authenticated (GH_TOKEN in Actions). Browser requests stay on
the Pages origin: release download redirects do not reliably allow browser CORS.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

from artifacts import FAP_TARGET, FIRMWARES
from build_config import HELP_FILE, data_directory, help_target, validate_version
from pack_fap import parse

ROOT = Path(__file__).resolve().parents[1]
MAX_ASSET = 2_000_000
NAMES = {'missile_cmd.fap', HELP_FILE, 'install-manifest.json'} | {
    f'missile_cmd-{firmware}.{extension}'
    for firmware in FIRMWARES for extension in ('fap', 'zip')}


def api_version(data):
    _, _, bodies, names = parse(data)
    magic, schema, api, hardware = struct.unpack_from('<IIIH', bodies[names.index('.fapmeta')])
    if (magic, schema, hardware) != (0x52474448, 1, 7):
        raise ValueError('Release contains an unsupported FAP')
    return f'{api >> 16}.{api & 65535}'


def file_record(name, data, prefix, version=None):
    return {'name': name, 'url': f'{prefix}/{name}', 'size': len(data),
            'target': FAP_TARGET if name.endswith('.fap') else help_target(version),
            'sha256': hashlib.sha256(data).hexdigest(),
            # Device CLI exposes MD5. SHA-256 is used for the download itself.
            'md5': hashlib.md5(data, usedforsecurity=False).hexdigest()}


def release_record(release, files):
    prefix = f'releases/{int(release["id"])}'
    manifest = json.loads(files['install-manifest.json']) if 'install-manifest.json' in files else None
    if 'install-manifest.json' in files and (not isinstance(manifest, dict) or
            manifest.get('schema') != 2 or not manifest.get('builds') or
            manifest.get('version') != release['tag_name'].removeprefix('v')):
        raise ValueError('Release manifest schema/version mismatch')
    builds = []
    if manifest:
        version = validate_version(manifest['version'])
        if manifest.get('data_directory') != data_directory(version):
            raise ValueError('Release manifest data directory differs from its version')
        for build in manifest['builds']:
            firmware = build['firmware']
            if firmware not in FIRMWARES or any(b['firmware'] == firmware for b in builds):
                raise ValueError('Invalid or duplicate firmware')
            records = []
            expected_names = [f'missile_cmd-{firmware}.fap', HELP_FILE]
            if [item['name'] for item in build['files']] != expected_names:
                raise ValueError('Manifest must contain the matching FAP and help file')
            for item in build['files']:
                record = file_record(item['name'], files[item['name']], prefix, version)
                if any(record[key] != item[key] for key in ('target', 'sha256')):
                    raise ValueError('Release asset does not match manifest')
                records.append(record)
            if api_version(files[expected_names[0]]) != build['api_version']:
                raise ValueError('Manifest API does not match FAP')
            builds.append({**build, 'files': records, 'data_version': version,
                           'data_directory': data_directory(version)})
    else:
        # Older releases predate build manifests. Read their API from the FAP,
        # but do not claim a firmware version using today's build-lock.json.
        for firmware in FIRMWARES:
            name = f'missile_cmd-{firmware}.fap'
            if name not in files and firmware == 'official':
                name = 'missile_cmd.fap'
            if name not in files:
                continue
            records = [file_record(name, files[name], prefix)]
            builds.append({'firmware': firmware, 'firmware_version': None,
                           'data_version': None, 'data_directory': None,
                           'api_version': api_version(files[name]), 'files': records})
    for build in builds:
        bundle = f'missile_cmd-{build["firmware"]}.zip'
        build['bundle_url'] = f'{prefix}/{bundle}' if bundle in files else None
    return {'tag': release['tag_name'], 'url': release['html_url'],
            'prerelease': release['prerelease'], 'published_at': release['published_at'],
            'builds': builds}


def download(asset, repository):
    url = asset['browser_download_url']
    if asset['name'] not in NAMES or not url.startswith(
            f'https://github.com/{repository}/releases/download/'):
        raise ValueError('Unexpected release asset URL/name')
    if not 0 < asset['size'] <= MAX_ASSET:
        raise ValueError('Invalid release asset size')
    # Authenticate only in the build, so private repositories also work without
    # exposing a token to visitors. gh handles GitHub's signed asset redirect.
    data = subprocess.check_output([
        'gh', 'api', f'repos/{repository}/releases/assets/{int(asset["id"])}',
        '-H', 'Accept: application/octet-stream'], timeout=90)
    if len(data) != asset['size'] or len(data) > MAX_ASSET:
        raise ValueError('Incomplete or oversized release download')
    digest = asset.get('digest')
    if digest and digest != 'sha256:' + hashlib.sha256(data).hexdigest():
        raise ValueError('GitHub asset checksum mismatch')
    return data


def build(output, repository):
    if not re.fullmatch(r'[\w.-]+/[\w.-]+', repository):
        raise ValueError('Invalid repository')
    # Only replace our generated directory, never an arbitrary user path.
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError('Output directory must be empty; remove the previous generated site first')
    shutil.copytree(ROOT / 'web', output, dirs_exist_ok=True)
    shutil.copy2(ROOT / 'screenshots/gameplay.png', output / 'gameplay.png')
    pages = json.loads(subprocess.check_output(
        ['gh', 'api', f'repos/{repository}/releases?per_page=100', '--paginate', '--slurp'], text=True))
    releases = []
    for release in sorted((r for page in pages for r in page if not r['draft']),
                          key=lambda r: r['published_at'], reverse=True):
        assets = [a for a in release['assets'] if a['name'] in NAMES]
        if not any(a['name'].endswith('.fap') for a in assets):
            continue
        files = {a['name']: download(a, repository) for a in assets}
        record = release_record(release, files)
        if not record['builds']:
            continue
        directory = output / 'releases' / str(int(release['id']))
        directory.mkdir(parents=True)
        for name, data in files.items():
            (directory / name).write_bytes(data)
        releases.append(record)
    (output / 'catalog.json').write_text(json.dumps(
        {'schema': 1, 'repository': repository, 'releases': releases}, indent=2) + '\n')
    (output / '.nojekyll').touch()
    print(f'Built {output} with {len(releases)} published releases')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/pages')
    parser.add_argument('--repository', default='LTDev-LLC/Missile-CMD')
    args = parser.parse_args()
    build(args.output, args.repository)
