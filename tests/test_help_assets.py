"""Independent help format, stable-ID, source inventory, and delivery checks."""
import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import Mock
import zipfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import artifacts
import help_assets
import sources
import upload
from build_config import help_asset, help_target
from test_firmware import fap


class HelpAssetsTests(unittest.TestCase):
    def setUp(self):
        self.catalogue = json.loads((help_assets.ROOT / 'assets/help.json').read_text())

    def test_index_checksums_and_stable_identity(self):
        packed, header = help_assets.encode(self.catalogue)
        self.assertEqual(help_assets.encode(json.loads(json.dumps(self.catalogue, indent=4))),
                         (packed, header))
        magic, identity, count, stride = struct.unpack_from('<4sIHH', packed)
        self.assertEqual((magic, count, stride), (b'MCH1', 52, 8))
        self.assertIn(f'0x{identity:08X}U', header)
        end = 12 + count * 8
        for i, page in enumerate(self.catalogue):
            offset, size, lines, crc = struct.unpack_from('<HBBI', packed, 12 + 8 * i)
            self.assertEqual(offset, end)
            data = packed[offset:offset + size]
            self.assertEqual(data, b''.join(line.encode() + b'\0' for line in page['lines']))
            self.assertEqual(lines, len(page['lines']))
            self.assertEqual(crc, zlib.crc32(data))
            self.assertLessEqual(size, 128)
            self.assertLessEqual(lines, 5)
            self.assertIn(f'McHelp{page["id"]} = {i},', header)
            end += size
        self.assertEqual(end, len(packed))
        self.catalogue[0]['lines'][0] += '.'
        changed, _ = help_assets.encode(self.catalogue)
        self.assertNotEqual(changed[4:8], packed[4:8])

    def test_invalid_pages_and_reordered_ids(self):
        for invalid in [[], [''], ['a'] * 6, ['x' * 128], ['line\0break'], ['é'], ['new\nline']]:
            with self.subTest(invalid=invalid):
                data = copy.deepcopy(self.catalogue)
                data[0]['lines'] = invalid
                with self.assertRaises(ValueError):
                    help_assets.encode(data)
        data = copy.deepcopy(self.catalogue)
        data[0]['lines'] = ['x' * 127]  # Exactly 128 bytes with its terminator is valid.
        help_assets.encode(data)
        for data in [self.catalogue[1:], list(reversed(self.catalogue)), self.catalogue * 2]:
            with self.assertRaises(ValueError):
                help_assets.encode(data)

    def test_device_and_host_inventory_covers_every_module_once(self):
        grouped = sources.sources('core', 'ui', 'app')
        actual = {str(path.relative_to(sources.ROOT)) for path in (sources.ROOT / 'src').glob('*.c')
                  if path.name != 'missile_cmd_core.c'}
        self.assertEqual(len(grouped), len(set(grouped)))
        self.assertEqual(set(grouped), actual)
        self.assertEqual(sources.sources('app'), ['src/app.c'])


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.version = '1.2.3-rc.1+build.5'
        (self.root / 'VERSION').write_text(self.version)
        (self.root / 'dist').mkdir()
        self.firmware = 'official'
        self.fap = self.root / 'dist/missile_cmd-official.fap'
        self.fap.write_bytes(fap())
        self.help = help_asset(self.root)
        self.help.parent.mkdir(parents=True)
        self.help.write_bytes(b'optional help fixture')
        self.report = {'firmware': 'official', 'version': '1.4.3', 'api_version': '87.1',
                       'app_version': self.version,
                       'sha256': artifacts.file_sha256(self.fap),
                       'help_file': self.help.name, 'help_sha256': artifacts.file_sha256(self.help)}
        artifacts.make_bundle(self.root, self.firmware)
        self.bundle = artifacts.bundle_path(self.root, self.firmware)
        self.report['bundle_sha256'] = artifacts.file_sha256(self.bundle)
        self.metadata = self.root / 'build_metadata/official/sdk.json'
        self.metadata.parent.mkdir(parents=True)
        self.metadata.write_text(json.dumps(self.report))

    def test_bundle_is_reproducible_and_uses_sd_paths(self):
        before = self.bundle.read_bytes()
        artifacts.make_bundle(self.root, self.firmware)
        self.assertEqual(self.bundle.read_bytes(), before)
        artifacts.verify_artifact(self.root, self.firmware)
        artifacts.verify_bundle(self.root, self.firmware)
        with zipfile.ZipFile(self.bundle) as archive:
            self.assertEqual(archive.namelist(), ['apps/Games/missile_cmd.fap',
                                                f'apps_data/missile_cmd/{self.version}/help.bin'])
            for info in archive.infolist():
                self.assertEqual(info.date_time, (1980, 1, 1, 0, 0, 0))
                self.assertEqual(info.external_attr >> 16, 0o100644)

    def test_post_upload_mismatch_stops_before_launch(self):
        class Operations:
            def __init__(self, storage):
                self.storage = storage

            def send_file_to_storage(self, target, source, force=False):
                self.storage.send(source, target)

        sdk = SimpleNamespace(FlipperStorageOperations=Operations)
        upload.verify_uploads(sdk)
        storage = Mock()
        storage.hash_local.return_value = storage.hash_flipper.return_value = 'matches'
        ops = sdk.FlipperStorageOperations(storage)
        ops.send_file_to_storage(artifacts.FAP_TARGET, self.fap)
        storage.send.assert_called_once_with(self.fap, artifacts.FAP_TARGET)
        storage.hash_flipper.return_value = 'bad transfer'
        launch = Mock()
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            ops.send_file_to_storage(help_target(self.version), self.help)
            launch()
        launch.assert_not_called()

    def test_tampering_is_rejected_before_upload(self):
        for path in (self.fap, self.help, self.bundle):
            with self.subTest(path=path):
                original = path.read_bytes()
                path.write_bytes(original + b'changed')
                with self.assertRaises(ValueError):
                    if path == self.bundle:
                        artifacts.verify_bundle(self.root, self.firmware)
                    else:
                        artifacts.verify_artifact(self.root, self.firmware)
                path.write_bytes(original)
        # Even a rehashed ZIP must contain the exact validated artifacts.
        with zipfile.ZipFile(self.bundle, 'w') as archive:
            archive.writestr('apps/Games/missile_cmd.fap', b'stale')
            archive.writestr(f'apps_data/missile_cmd/{self.version}/help.bin', self.help.read_bytes())
        self.report['bundle_sha256'] = artifacts.file_sha256(self.bundle)
        self.metadata.write_text(json.dumps(self.report))
        with self.assertRaisesRegex(ValueError, 'stale'):
            artifacts.verify_bundle(self.root, self.firmware)


if __name__ == '__main__':
    unittest.main()
