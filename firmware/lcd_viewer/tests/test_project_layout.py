"""Regression checks for the exact Arduino source boundary and pinned dependency."""
from pathlib import Path
import hashlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import project_layout as layout


class SourceBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def put(self, name, content='// source\n'):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8')
        return path

    def test_nested_sources_compile_but_tests_and_sdk_do_not(self):
        for name in ('lcd_viewer.ino', 'src/ui/view.cpp', 'src/ui/view.h',
                     'tests/test.cpp', '.build/sdk/bits/extc++.h', 'README.md'):
            self.put(name)
        self.assertEqual(set(layout.source_files(self.root)),
                         {'lcd_viewer.ino', 'src/ui/view.cpp', 'src/ui/view.h'})

    def test_added_deleted_and_changed_sources_change_manifest(self):
        self.put('lcd_viewer.ino')
        source = self.put('src/view.cpp', 'before')
        before = layout.source_hashes(self.root)
        source.write_text('after')
        self.assertNotEqual(before['src/view.cpp'], layout.source_hashes(self.root)['src/view.cpp'])
        source.unlink()
        self.put('src/ui/new.cpp')
        self.assertEqual(set(layout.source_hashes(self.root)), {'lcd_viewer.ino', 'src/ui/new.cpp'})

    def test_unsafe_arduino_filename_rejected(self):
        self.put('lcd_viewer.ino')
        self.put('src/extc++.h')
        with self.assertRaises(ValueError):
            layout.source_files(self.root)

    def test_nested_ino_rejected(self):
        self.put('lcd_viewer.ino')
        self.put('src/extra.ino')
        with self.assertRaises(ValueError):
            layout.source_files(self.root)

    def test_no_matching_sketch_entry_rejected(self):
        self.put('src/view.cpp')
        with self.assertRaises(ValueError):
            layout.source_files(self.root)

    def test_dependency_hash_order_is_identical_across_platforms(self):
        for name in ('Z.h', 'a.h', 'src/lv_conf.h'):
            self.put(name, name)
        expected = hashlib.sha256()
        for name in ('Z.h', 'a.h', 'src/lv_conf.h'):
            expected.update(name.encode())
            expected.update(b'\0')
            expected.update(hashlib.sha256(name.encode()).digest())
        self.assertEqual(layout.dependency_fingerprint(self.root)['tree_sha256'], expected.hexdigest())


if __name__ == '__main__':
    unittest.main()
