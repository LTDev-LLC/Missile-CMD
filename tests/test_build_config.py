"""VERSION changes must reach the C app and delivered help without source edits."""
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_config
import help_assets


class VersionPathsTests(unittest.TestCase):
    def test_version_change_moves_help_and_regenerates_compiled_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'assets').mkdir()
            (root / 'assets/help.json').write_text(json.dumps(
                json.loads((help_assets.ROOT / 'assets/help.json').read_text())))
            old_file = None
            for version in ['1.2.3', '2.0.0-rc.2+build.007']:
                (root / 'VERSION').write_text(version + '\n')
                help_assets.generate(root)
                artifact = build_config.help_asset(root)
                self.assertEqual(artifact, root / f'dist/{version}/help.bin')
                self.assertTrue(artifact.is_file())
                if old_file:
                    self.assertTrue(old_file.exists())
                old_file = artifact
                # Expand the actual C headers: the app's data paths must equal
                # the Python packaging/install paths, including the full suffix.
                source = '#define APP_DATA_PATH(x) "/ext/apps_data/missile_cmd/" x\n#include "data_paths.h"\nMC_HELP_PATH\nMC_APP_VERSION\nMC_HELP_ASSET_PATH\n'
                result = subprocess.check_output(['cc', '-E', '-P', '-x', 'c',
                    '-I' + str(root / 'build/generated'),
                    '-I' + str(help_assets.ROOT / 'src/include'), '-'], input=source, text=True)
                lines = [''.join(json.loads(part) for part in re.findall(r'"[^"\\]*"', line))
                         for line in result.strip().splitlines()]
                self.assertEqual(lines, [build_config.help_target(version), version,
                                        f'dist/{version}/help.bin'])

    def test_rejects_unsafe_or_noncanonical_version_components(self):
        for version in ['', 'v1.2.3', '1.2', '01.2.3', '1.2.3-01', '1.2.3+build..1',
                        '../1.2.3', '1.2.3/extra', '1.2.3\\extra', '1.2.3\n',
                        '1.2.3-$(echo bad)', '1.2.3-' + 'a' * 250]:
            with self.subTest(version=version), self.assertRaises(ValueError):
                build_config.validate_version(version)
        for version in ['0.0.0', '1.2.3-alpha.0', '1.2.3-01alpha', '1.2.3+001']:
            self.assertEqual(build_config.validate_version(version), version)


if __name__ == '__main__':
    unittest.main()
