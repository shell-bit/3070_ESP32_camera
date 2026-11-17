"""Keep the private embedded setup resource out of local source distributions."""
from pathlib import Path
import sys
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(PROJECT / 'tools'))
if (PROJECT / 'tools/package_lcd_ui.py').is_file():
    import package_lcd_ui as package
else:
    package = None


@unittest.skipIf(package is None, 'Local package_lcd_ui.py tool is unavailable')
class PrivateSetupDistributionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.project = Path(self.temp.name)
        self.lcd = self.project / 'firmware/lcd_viewer'
        self.put('firmware/lcd_viewer/lcd_viewer.ino', '// synthetic sketch\n')
        for name in ('tools/package_lcd_ui.py', 'tools/export_lcd_arduino.py',
                     'docs/development/lcd-viewer-architecture.md', 'arduino/README.md'):
            self.put(name, 'Synthetic fixture\n')

    def put(self, name, content):
        path = self.project / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8')
        return path

    def test_private_production_resource_is_refused_even_with_clean_source_map(self):
        self.put('firmware/lcd_viewer/setup.json', '{"synthetic":true}\n')
        with self.assertRaisesRegex(SystemExit, 'private setup.json'):
            package.distribution_files(project=self.project, lcd=self.lcd,
                                       sources={'lcd_viewer.ino': '0' * 64})

    def test_private_source_map_is_refused_even_without_local_resource(self):
        with self.assertRaisesRegex(SystemExit, 'private setup.json'):
            package.distribution_files(project=self.project, lcd=self.lcd,
                                       sources={'lcd_viewer.ino': '0' * 64,
                                                'setup.json': '1' * 64})

    def test_private_setup_refusal_does_not_create_archive(self):
        self.put('firmware/lcd_viewer/setup.json', '{"synthetic":true}\n')
        destination = self.project / 'source.zip'
        with self.assertRaisesRegex(SystemExit, 'private setup.json'):
            package.write_source_archive(destination, project=self.project, lcd=self.lcd)
        self.assertFalse(destination.exists())

    def test_private_setup_refusal_preserves_existing_archive(self):
        self.put('firmware/lcd_viewer/setup.json', '{"synthetic":true}\n')
        destination = self.project / 'source.zip'
        destination.write_bytes(b'existing synthetic archive')
        with self.assertRaisesRegex(SystemExit, 'private setup.json'):
            package.write_source_archive(destination, project=self.project, lcd=self.lcd)
        self.assertEqual(destination.read_bytes(), b'existing synthetic archive')

    def test_distribution_without_private_setup_still_selects_sources(self):
        self.put('firmware/lcd_viewer/setup_data.S', '// synthetic assembly\n')
        self.put('firmware/lcd_viewer/dependencies.lock.json', '{}\n')
        selected = package.distribution_files(project=self.project, lcd=self.lcd)
        self.assertEqual(set(selected), {
            'firmware/lcd_viewer/lcd_viewer.ino',
            'firmware/lcd_viewer/setup_data.S',
            'firmware/lcd_viewer/dependencies.lock.json',
            'tools/package_lcd_ui.py',
            'tools/export_lcd_arduino.py',
            'docs/development/lcd-viewer-architecture.md',
            'arduino/README.md',
        })


if __name__ == '__main__':
    unittest.main()
