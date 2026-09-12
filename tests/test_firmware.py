import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import tarfile
import unittest
import zipfile
import hashlib
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import firmware
import install
from pack_fap import SECTION, compact, parse
from test_pack_fap import fixture
from build_config import help_asset


def fap(api='87.1', hardware=7):
    data = bytearray(fixture())
    header, sections, _, names = parse(data)
    index = names.index('.fapmeta')
    major, minor = map(int, api.split('.'))
    manifest = struct.pack('<IIIH', 0x52474448, 1, major << 16 | minor, hardware)
    sections[index][4:6] = len(data), len(manifest)
    SECTION.pack_into(data, header[6] + index * SECTION.size, *sections[index])
    data.extend(manifest)
    return bytes(data)


def release(name, tag):
    filename = f'flipper-z-f7-sdk-{tag}.zip'
    return {'tag_name': tag, 'assets': [
        {'name': 'firmware.tgz', 'browser_download_url': 'unused'},
        {'name': filename, 'browser_download_url':
         f'https://github.com/{firmware.REPOSITORIES[name]}/releases/download/{tag}/{filename}'}]}


def response(data):
    return io.BytesIO(json.dumps(data).encode())


class FirmwareTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'VERSION').write_text('1.2.3\n')
        (self.root / 'build-lock.json').write_text((firmware.ROOT / 'build-lock.json').read_text())
        (self.root / 'assets').mkdir()
        (self.root / 'assets/help.json').write_text((firmware.ROOT / 'assets/help.json').read_text())
        self.patches = [patch.object(firmware, 'ROOT', self.root),
                        patch.object(install, 'ROOT', self.root),
                        patch.dict(os.environ, {'SDK_ROOT': str(self.root / 'SDK cache'),
                                               'FBT_TOOLCHAIN_PATH': str(self.root / 'toolchain'),
                                               'SDK_VERSION': 'latest'}, clear=True)]
        for p in self.patches:
            p.start()
            self.addCleanup(p.stop)

    def sdk(self, name, version, api):
        env = firmware.environment(name)
        sdk = Path(env['UFBT_HOME']) / 'current'
        sdk.mkdir(parents=True)
        (sdk / 'components.json').write_text(json.dumps({'meta': {'version': version, 'hw_target': 'f7'}}))
        symbols = sdk / 'sdk_headers/f7_sdk/targets/f7/api_symbols.csv'
        symbols.parent.mkdir(parents=True)
        symbols.write_text(f'entry,status,name,type,params\nVersion,+,{api},,\n')
        uploader = sdk / 'scripts/runfap.py'
        uploader.parent.mkdir()
        uploader.touch()
        runtime = Path(env['FBT_TOOLCHAIN_PATH']) / 'toolchain/current/bin/python3'
        runtime.parent.mkdir(parents=True, exist_ok=True)
        runtime.touch()
        (runtime.parent.parent / 'VERSION').write_text('39')
        return env

    def test_release_resolution_uses_each_custom_repository(self):
        for name, tag in [('unleashed', 'unlshd-092'), ('momentum', 'mntm-012')]:
            with self.subTest(name=name), patch.object(firmware, 'urlopen', return_value=response(release(name, tag))) as fetch:
                version, url = firmware.release_sdk(name)
                self.assertEqual(version, tag)
                self.assertIn(f'/{firmware.REPOSITORIES[name]}/releases/download/{tag}/', url)
                self.assertEqual(fetch.call_args.args[0].full_url,
                                 f'https://api.github.com/repos/{firmware.REPOSITORIES[name]}/releases/latest')

    def test_official_index_selects_latest_f7_sdk(self):
        def entry(version, timestamp):
            base = f'https://update.flipperzero.one/builds/firmware/{version}'
            return {'version': version, 'timestamp': timestamp, 'files': [
                {'type': 'sdk_zip', 'target': target, 'url': f'{base}/flipper-z-{target}-sdk-{version}.zip'}
                for target in ('f18', 'f7')]}
        directory = {'channels': [{'id': 'release', 'versions': [entry('1.4.2', 1), entry('1.4.3', 2)]}]}
        for requested, expected in [('latest', '1.4.3'), ('1.4.2', '1.4.2')]:
            with patch.object(firmware, 'urlopen', return_value=response(directory)):
                version, url = firmware.release_sdk('official', requested)
                self.assertEqual(version, expected)
                self.assertIn(f'/flipper-z-f7-sdk-{expected}.zip', url)

    def test_rejects_missing_ambiguous_or_foreign_sdk_assets(self):
        valid = release('momentum', 'mntm-012')
        bad_cases = [dict(valid, assets=[]), dict(valid, assets=[valid['assets'][1]] * 2),
                     dict(valid, tag_name='unlshd-092')]
        foreign = json.loads(json.dumps(valid))
        foreign['assets'][1]['browser_download_url'] = 'https://example.com/sdk.zip'
        for data in [*bad_cases, foreign]:
            with patch.object(firmware, 'urlopen', return_value=response(data)):
                with self.assertRaises(ValueError):
                    firmware.release_sdk('momentum')

    def test_isolates_sdk_homes_and_rejects_dotenv_switch(self):
        with patch.dict(os.environ, {'UFBT_HOME': '/some/other/sdk'}):
            homes = [firmware.environment(name)['UFBT_HOME'] for name in firmware.REPOSITORIES]
        self.assertEqual(len(set(homes)), 3)
        self.assertTrue(all(str(self.root) in home for home in homes))
        (self.root / '.env').write_text('UFBT_HOME=/wrong/sdk\n')
        with self.assertRaisesRegex(ValueError, '.env overrides UFBT_HOME'):
            firmware.environment('unleashed')

    def test_sdk_identity_checked_even_when_api_matches(self):
        env = self.sdk('momentum', 'mntm-012', '87.1')
        self.assertEqual(firmware.sdk_info('momentum', env)['api_version'], '87.1')
        with self.assertRaisesRegex(ValueError, 'SDK does not match'):
            firmware.sdk_info('official', env)

    def test_reuses_verified_cache_and_updates_from_verified_archive(self):
        env = self.sdk('unleashed', 'unlshd-092', '88.4')
        entry = firmware.build_lock()['firmwares']['unleashed']
        archive = self.root / 'verified.zip'
        with patch.object(firmware, 'release_sdk', return_value=('unlshd-092', entry['url'])) as fetch, \
                patch.object(firmware, 'download_sdk', return_value=(archive, entry)), \
                patch.object(firmware, 'verify_sdk_tree', return_value=True), \
                patch.object(firmware, 'run') as run:
            firmware.ensure_sdk('unleashed', env)
            fetch.assert_not_called()
            run.assert_not_called()
            firmware.ensure_sdk('unleashed', env, update=True)
            fetch.assert_called_once_with('unleashed', 'latest')
            self.assertEqual(run.call_args.args[0], ['ufbt', 'update', '--hw-target', 'f7', '--local', str(archive)])

    def test_pin_rejects_wrong_installed_version(self):
        env = self.sdk('unleashed', 'unlshd-091', '88.3')
        entry = firmware.build_lock()['firmwares']['unleashed']
        with patch.dict(os.environ, {'SDK_VERSION': 'unlshd-092'}), \
                patch.object(firmware, 'download_sdk', return_value=(self.root / 'sdk.zip', entry)), \
                patch.object(firmware, 'verify_sdk_tree', return_value=False), \
                patch.object(firmware, 'run'):
            with self.assertRaisesRegex(ValueError, 'version does not match'):
                firmware.ensure_sdk('unleashed', env)

    def test_archive_hash_and_installed_contents_are_checked(self):
        env = firmware.environment('official')
        archive = self.root / 'tiny.zip'
        with zipfile.ZipFile(archive, 'w') as bundle:
            bundle.writestr('components.json', 'original')
        data = archive.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        with patch.object(firmware, 'urlopen', return_value=io.BytesIO(data)):
            downloaded, receipt = firmware.download_sdk('official', '1.4.3', 'https://sdk', env, digest)
        self.assertEqual(receipt['sha256'], digest)
        sdk = self.root / 'extracted'
        sdk.mkdir()
        (sdk / 'components.json').write_text('original')
        self.assertTrue(firmware.verify_sdk_tree(downloaded, sdk))
        (sdk / 'components.json').write_text('tampered')
        self.assertFalse(firmware.verify_sdk_tree(downloaded, sdk))
        downloaded.write_bytes(b'corrupt cache')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            firmware.download_sdk('official', '1.4.3', 'https://sdk', env, digest)

    def test_default_uses_lock_and_bootstrap_mismatch_fails(self):
        for deployed in (False, True):
            status = {'ufbt_version': '0.2.5'}
            if not deployed:
                status['error'] = 'SDK is not deployed'
            result = subprocess.CompletedProcess(['ufbt'], 0 if deployed else 1, json.dumps(status), '')
            with self.subTest(deployed=deployed), patch.object(firmware.subprocess, 'run', return_value=result):
                with self.assertRaisesRegex(ValueError, 'uFBT version differs'):
                    firmware.verify_bootstrap(firmware.environment('official'))
        with patch.object(firmware, 'release_sdk') as fetch:
            self.assertEqual(firmware.resolve_lock()['firmwares']['official']['version'], '1.4.3')
            fetch.assert_not_called()

    def test_bootstrap_accepts_missing_sdk_but_rejects_other_status_errors(self):
        cases = [(0, None, True), (1, 'SDK is not deployed', True),
                 (1, 'Unable to read SDK state', False), (2, 'SDK is not deployed', False)]
        for code, error, accepted in cases:
            status = {'ufbt_version': firmware.build_lock()['ufbt']}
            if error:
                status['error'] = error
            result = subprocess.CompletedProcess(['ufbt'], code, json.dumps(status), '')
            with self.subTest(code=code, error=error), patch.object(firmware.subprocess, 'run', return_value=result):
                env = firmware.environment('official')
                self.assertFalse((Path(env['UFBT_HOME']) / 'current').exists())
                if accepted:
                    self.assertEqual(firmware.verify_bootstrap(env), status['ufbt_version'])
                else:
                    with self.assertRaises(subprocess.CalledProcessError):
                        firmware.verify_bootstrap(env)

    def test_toolchain_cache_checks_files_and_links(self):
        archive = self.root / 'compiler.tar.gz'
        with tarfile.open(archive, 'w:gz') as bundle:
            member = tarfile.TarInfo('compiler/bin/gcc')
            member.size = 8
            bundle.addfile(member, io.BytesIO(b'compiler'))
            link = tarfile.TarInfo('compiler/bin/cc')
            link.type = tarfile.SYMTYPE
            link.linkname = 'gcc'
            bundle.addfile(link)
        installed = self.root / 'installed'
        (installed / 'bin').mkdir(parents=True)
        (installed / 'bin/gcc').write_bytes(b'compiler')
        (installed / 'bin/cc').symlink_to('gcc')
        self.assertTrue(firmware.verify_toolchain_tree(archive, installed))
        (installed / 'bin/gcc').write_bytes(b'tampered')
        self.assertFalse(firmware.verify_toolchain_tree(archive, installed))
        (installed / 'bin/gcc').write_bytes(b'compiler')
        (installed / 'bin/cc').unlink()
        (installed / 'bin/cc').symlink_to('foreign')
        self.assertFalse(firmware.verify_toolchain_tree(archive, installed))

    def test_toolchain_hard_links_verify_without_rewinding(self):
        archive = self.root / 'hardlinks.tar.gz'
        installed = self.root / 'installed'
        installed.mkdir()
        with tarfile.open(archive, 'w:gz') as bundle:
            for name, data in [('gcc', b'compiler'), ('padding', b'x' * 131072)]:
                member = tarfile.TarInfo('compiler/' + name)
                member.size = len(data)
                bundle.addfile(member, io.BytesIO(data))
                (installed / name).write_bytes(data)
            for name, target in [('cc', 'gcc'), ('c++', 'cc')]:
                member = tarfile.TarInfo('compiler/' + name)
                member.type = tarfile.LNKTYPE
                member.linkname = 'compiler/' + target
                bundle.addfile(member)
                (installed / name).write_bytes(b'compiler')

        class ForwardOnly(io.BytesIO):
            def seek(self, offset, whence=0):
                previous = self.tell()
                position = super().seek(offset, whence)
                if position < previous:
                    raise AssertionError('Toolchain verification rewound the compressed archive')
                return position

        archive_open = tarfile.open
        def open_forward(path, mode):
            return archive_open(path, mode, fileobj=ForwardOnly(path.read_bytes()))
        with patch.object(firmware.tarfile, 'open', side_effect=open_forward):
            self.assertTrue(firmware.verify_toolchain_tree(archive, installed))
        (installed / 'cc').write_bytes(b'tampered')
        self.assertFalse(firmware.verify_toolchain_tree(archive, installed))
        (installed / 'cc').write_bytes(b'compiler')
        (installed / 'c++').unlink()
        (installed / 'c++').symlink_to('gcc')
        self.assertFalse(firmware.verify_toolchain_tree(archive, installed))

    def test_clean_build_comparison_rejects_different_outputs(self):
        artifact = self.root / 'dist/missile_cmd-official.fap'
        artifact.parent.mkdir()
        metadata = self.root / 'build_metadata/official'
        metadata.mkdir(parents=True)
        for payloads, identical in [([b'a', b'a'], True), ([b'a', b'b'], False)]:
            def write(*args):
                artifact.write_bytes(payloads.pop(0))
                help_asset(self.root).parent.mkdir(parents=True, exist_ok=True)
                help_asset(self.root).write_bytes(b'help')
                (artifact.parent / 'missile_cmd-official.zip').write_bytes(b'bundle')
            with patch.object(firmware, 'run') as clean, patch.object(firmware, 'build', side_effect=write):
                if identical:
                    firmware.reproducibility_check('official', {}, {})
                    self.assertTrue(json.loads((metadata / 'reproducibility.json').read_text())['identical'])
                else:
                    with self.assertRaisesRegex(ValueError, 'two clean packed builds differ'):
                        firmware.reproducibility_check('official', {}, {})
                self.assertEqual(clean.call_count, 2)
                self.assertTrue(all(call.args[0] == ['ufbt', '-c'] for call in clean.call_args_list))
    def test_fap_api_and_hardware_must_match_sdk(self):
        firmware.verify_api(fap('88.4'), '88.4')
        for data in [fap('87.1'), fap('88.4', 18)]:
            with self.assertRaises(ValueError):
                firmware.verify_api(data, '88.4')

    def test_build_and_install_use_the_selected_packed_artifact(self):
        env = self.sdk('momentum', 'mntm-012', '87.1')
        env['FLIP_PORT'] = '/dev/test port'
        (self.root / 'VERSION').write_text('1.2.3\n')
        raw = self.root / 'dist/missile_cmd.fap'
        raw.parent.mkdir()
        raw.write_bytes(b'stale artifact from another firmware')
        def build_step(command, passed_env):
            self.assertEqual(passed_env['UFBT_HOME'], env['UFBT_HOME'])
            if command[0] == 'ufbt':
                self.assertFalse(raw.exists())
                self.assertIn('STRICT_FAP_IMPORT_CHECK=1', command)
                raw.write_bytes(fap())
            else:
                raw.write_bytes(compact(raw.read_bytes()))
                Path(command[command.index('--metadata-dir') + 1]).mkdir(parents=True)
        with patch.object(firmware, 'run', side_effect=build_step), patch.object(firmware, 'verify_bootstrap', return_value='0.2.6'):
            firmware.build('momentum', env, firmware.sdk_info('momentum', env))
        artifact = firmware.artifact_path('momentum')
        self.assertEqual(artifact.read_bytes(), compact(fap()))
        with patch.object(install.subprocess, 'run') as upload:
            install.install('momentum', env)
            command = upload.call_args.args[0]
            self.assertIn(str(artifact), command)
            self.assertEqual(command[command.index('-s') + 1:command.index('-t')],
                             [str(artifact), str(self.root / 'dist/1.2.3/help.bin')])
            self.assertEqual(command[command.index('-t') + 1:],
                             ['/ext/apps/Games/missile_cmd.fap',
                              '/ext/apps_data/missile_cmd/1.2.3/help.bin'])
            self.assertIn(str(Path(env['UFBT_HOME']) / 'current/scripts/runfap.py'), command)
            self.assertIn('/dev/test port', command)
        report_path = self.root / 'build_metadata/momentum/sdk.json'
        saved_report = report_path.read_text()
        report = json.loads(saved_report)
        report['firmware'] = 'official'
        report_path.write_text(json.dumps(report))
        with self.assertRaisesRegex(ValueError, 'Artifact or SDK changed'):
            install.upload_command('momentum', env)
        report_path.write_text(saved_report)
        artifact.write_bytes(artifact.read_bytes() + b'corrupt')
        with self.assertRaisesRegex(ValueError, 'Artifact or SDK changed'):
            install.upload_command('momentum', env)


if __name__ == '__main__':
    unittest.main()
