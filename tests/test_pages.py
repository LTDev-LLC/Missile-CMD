"""Release-to-site contract tests, including older and malformed release assets."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import artifacts
import build_pages
from test_firmware import fap
from build_config import data_directory, help_asset, help_target


class PagesTests(unittest.TestCase):
    def setUp(self):
        self.release = {'id': 123, 'tag_name': 'v1.2.3', 'prerelease': False,
                        'published_at': '2026-09-11T00:00:00Z',
                        'html_url': 'https://github.com/example/game/releases/tag/v1.2.3'}
        self.data = fap()
        self.files = {'missile_cmd-official.fap': self.data, 'help.bin': b'help'}
        self.manifest = {'schema': 2, 'version': '1.2.3',
                         'data_directory': data_directory('1.2.3'), 'builds': [{
            'firmware': 'official', 'firmware_version': '1.4.3', 'api_version': '87.1',
            'files': [{'name': name, 'target': target,
                       'sha256': hashlib.sha256(self.files[name]).hexdigest()}
                      for name, target in [('missile_cmd-official.fap', artifacts.FAP_TARGET),
                                           ('help.bin', help_target('1.2.3'))]]}]}

    def test_legacy_alias_is_official_only_without_invented_firmware_version(self):
        release = build_pages.release_record(self.release, {'missile_cmd.fap': self.data})
        self.assertEqual(len(release['builds']), 1)
        build = release['builds'][0]
        self.assertEqual(build['firmware'], 'official')
        self.assertIsNone(build['firmware_version'])
        self.assertEqual(build['api_version'], '87.1')
        self.assertEqual(len(build['files']), 1)
        self.assertEqual(build['files'][0]['url'], 'releases/123/missile_cmd.fap')

    def test_manifest_preserves_actual_build_and_matching_help(self):
        self.files['install-manifest.json'] = json.dumps(self.manifest).encode()
        build = build_pages.release_record(self.release, self.files)['builds'][0]
        self.assertEqual(build['firmware_version'], '1.4.3')
        self.assertEqual(build['files'][1]['target'], '/ext/apps_data/missile_cmd/1.2.3/help.bin')
        self.assertEqual(build['data_version'], '1.2.3')
        self.assertEqual(build['files'][0]['sha256'], hashlib.sha256(self.data).hexdigest())
        self.assertEqual(build['files'][0]['md5'], hashlib.md5(self.data).hexdigest())

    def test_rejects_missing_help_checksum_api_version_and_path_mismatches(self):
        mutations = [lambda m: m.update(version='1.2.4'),
                     lambda m: m.clear(),
                     lambda m: m.update(data_directory='/ext/apps_data/missile_cmd/wrong'),
                     lambda m: m['builds'][0]['files'][1].update(target=help_target('1.2.4')),
                     lambda m: m['builds'][0].update(api_version='88.0'),
                     lambda m: m['builds'][0]['files'][0].update(target='/ext/wrong.fap'),
                     lambda m: m['builds'][0]['files'][1].update(sha256='0' * 64),
                     lambda m: m['builds'][0]['files'].pop(),
                     lambda m: m['builds'].append(copy.deepcopy(m['builds'][0]))]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                manifest = copy.deepcopy(self.manifest)
                mutate(manifest)
                files = {**self.files, 'install-manifest.json': json.dumps(manifest).encode()}
                with self.assertRaises(ValueError):
                    build_pages.release_record(self.release, files)
        files = {'missile_cmd-official.fap': self.data,
                 'install-manifest.json': json.dumps(self.manifest).encode()}
        with self.assertRaises(KeyError):
            build_pages.release_record(self.release, files)

    def test_build_paginates_skips_drafts_and_uses_release_ids_for_paths(self):
        release = {**self.release, 'tag_name': '../../unsafe/tag', 'draft': False,
                   'assets': [{'name': 'missile_cmd.fap'}]}
        draft = {**release, 'id': 124, 'draft': True}
        newer = {**release, 'id': 125, 'tag_name': 'v2.0.0-beta.1',
                 'prerelease': True, 'published_at': '2026-09-12T00:00:00Z'}
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(build_pages.subprocess, 'check_output', return_value=json.dumps([[draft, release], [newer]])) as gh, \
                patch.object(build_pages, 'download', return_value=self.data):
            output = Path(directory)
            build_pages.build(output, 'example/game')
            catalog = json.loads((output / 'catalog.json').read_text())
            self.assertEqual([r['tag'] for r in catalog['releases']], ['v2.0.0-beta.1', '../../unsafe/tag'])
            self.assertTrue((output / 'releases/123/missile_cmd.fap').exists())
            self.assertIn('--paginate', gh.call_args.args[0])
            with self.assertRaises(ValueError):
                build_pages.build(output, 'example/game')

    def test_prepare_release_publishes_verified_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'dist').mkdir()
            help_file = help_asset(root, '1.2.3')
            help_file.parent.mkdir()
            help_file.write_bytes(b'help')
            for firmware in artifacts.FIRMWARES:
                (root / f'dist/missile_cmd-{firmware}.fap').write_bytes(self.data)
                metadata = root / f'build_metadata/{firmware}/sdk.json'
                metadata.parent.mkdir(parents=True)
                metadata.write_text(json.dumps({'version': firmware + '-version', 'api_version': '87.1'}))
            with patch.object(artifacts, 'verify_artifact') as verify, patch.object(artifacts, 'verify_bundle'):
                artifacts.prepare_release(root, '1.2.3')
                self.assertEqual(verify.call_count, 3)
            manifest = json.loads((root / 'dist/install-manifest.json').read_text())
            self.assertEqual(manifest['version'], '1.2.3')
            self.assertEqual(manifest['schema'], 2)
            self.assertEqual(manifest['data_directory'], '/ext/apps_data/missile_cmd/1.2.3')
            self.assertEqual([b['firmware_version'] for b in manifest['builds']],
                             [f + '-version' for f in artifacts.FIRMWARES])
            self.assertEqual(manifest['builds'][0]['files'][1]['sha256'], hashlib.sha256(b'help').hexdigest())

    def test_download_rejects_untrusted_names_and_origins_before_request(self):
        for name, url in [('evil.fap', 'https://github.com/example/game/releases/download/v1/evil.fap'),
                          ('missile_cmd.fap', 'https://example.com/file')]:
            with self.assertRaises(ValueError):
                build_pages.download({'name': name, 'browser_download_url': url, 'size': 1}, 'example/game')


if __name__ == '__main__':
    unittest.main()
