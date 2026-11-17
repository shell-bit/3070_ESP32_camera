"""Camera setup helper tests; upload is checked without a connected board."""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


TOOLS = Path(__file__).resolve().parents[1] / 'tools'
TOKEN = 'fixture-camera-token-0123+/=-._~'


class FlashCameraConfigTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / 'lcd_viewer'
        self.root.mkdir()
        self.setup = self.root / 'camera_setup.json'
        self.header = self.root / 'camera_setup.local.h'
        (self.root / 'lcd_viewer.ino').write_text('// sketch\n', encoding='utf-8')

    def write_setup(self, host='camera-01.local', token=TOKEN):
        self.setup.write_text(json.dumps({'camera_host': host, 'token': token}), encoding='utf-8')

    def run_helper(self, *args):
        tools = self.root / 'tools'
        shutil.copytree(TOOLS, tools, dirs_exist_ok=True)
        return subprocess.run([sys.executable, '-B', str(tools / 'flash_camera_config.py'), *args],
                              cwd=self.root.parent, capture_output=True, text=True)

    def test_generates_private_header_and_new_revision_for_same_config(self):
        self.write_setup()
        revisions = set()
        for _ in range(2):
            result = self.run_helper()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn(TOKEN, result.stdout + result.stderr)
            header = self.header.read_text(encoding='utf-8')
            self.assertIn('namespace lcd', header)
            self.assertIn('cameraSetupJson[] = R"camera({"camera_host":"camera-01.local",'
                          '"token":"' + TOKEN + '"})camera";', header)
            revisions.add(re.search(r'cameraSetupRevision\[\] = "([0-9a-f]{32})";', header)[1])
            if os.name == 'posix':
                self.assertEqual(stat.S_IMODE(self.header.stat().st_mode), 0o600)
        self.assertEqual(len(revisions), 2)

    def test_accepts_ipv4_and_maximum_token_length(self):
        self.write_setup('192.168.42.172', 'x' * 128)
        result = self.run_helper()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('192.168.42.172', self.header.read_text(encoding='utf-8'))

    def test_invalid_input_preserves_header_and_does_not_attempt_cli(self):
        self.header.write_text('keep me', encoding='utf-8')
        valid = f'"camera_host":"camera.local","token":"{TOKEN}"'
        entries = [
            ('duplicate key', '{' + valid + ',"token":"other"}'),
            ('unexpected key', '{' + valid + f',"{TOKEN}":true}}'),
            ('missing key', f'{{"token":"{TOKEN}"}}'),
            ('invalid JSON', '{' + valid),
            ('not an object', '[' + json.dumps(TOKEN) + ']'),
        ]
        entries += [(host or 'empty host', json.dumps({'camera_host': host, 'token': TOKEN}))
                    for host in ('', 'https://camera.local', 'camera.local:80', 'bad..name',
                                 '-host.local', 'bad_name', 'a' * 64 + '.local', '2001:db8::1', '10.0.0.999')]
        entries += [(label, json.dumps({'camera_host': 'camera.local', 'token': token}))
                    for label, token in [('empty token', ''), ('short token', 'x' * 23),
                                         ('long token', 'x' * 129), ('whitespace', TOKEN + '\n'),
                                         ('quote', TOKEN + '"'), ('wrong type', None)]]
        for label, entry in entries:
            with self.subTest(label=label):
                self.setup.write_text(entry, encoding='utf-8')
                result = self.run_helper('--port', 'COM5', '--arduino-cli', 'no-such-arduino-cli')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('camera_setup.json', result.stderr)
                self.assertNotIn('no-such-arduino-cli', result.stderr)
                self.assertNotIn(TOKEN, result.stdout + result.stderr)
                self.assertEqual(self.header.read_text(encoding='utf-8'), 'keep me')

    def test_upload_uses_pinned_board_and_private_stage_and_redacts_diagnostics(self):
        sys.path.insert(0, str(TOOLS))
        self.addCleanup(sys.path.remove, str(TOOLS))
        import flash_camera_config as flash

        self.write_setup()
        self.header.write_text('existing IDE header', encoding='utf-8')
        (self.root / 'tests').mkdir()
        (self.root / 'tests/test.cpp').touch()
        libraries = self.root.parent / 'libraries'
        cli_path = str(self.root.parent / 'arduino cli')
        config_path = str(self.root.parent / 'cli.yml')
        staged_sketches = []

        def arduino_cli(command, **kwargs):
            self.assertEqual(command[0], cli_path)
            self.assertEqual(command[command.index('--config-file') + 1], config_path)
            if 'core' in command:
                return subprocess.CompletedProcess(command, 0, json.dumps({
                    'platforms': [{'id': 'esp32:esp32', 'installed_version': '3.3.12'}],
                }))
            self.assertIn('compile', command)
            self.assertIn('--upload', command)
            self.assertEqual(command[command.index('--port') + 1], 'COM5')
            self.assertEqual(command[command.index('--protocol') + 1], 'serial')
            self.assertEqual(command[command.index('--fqbn') + 1],
                             'esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,'
                             'PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,EraseFlash=none')
            self.assertEqual(command[command.index('--library') + 1], str(libraries / 'lvgl'))
            staged = next(Path(arg) for arg in command if Path(arg).name == 'lcd_viewer')
            staged_sketches.append(staged)
            self.assertNotEqual(staged, self.root)
            self.assertEqual({p.name for p in staged.iterdir()}, {'lcd_viewer.ino', 'camera_setup.local.h'})
            self.assertIn(TOKEN, (staged / 'camera_setup.local.h').read_text(encoding='utf-8'))
            build = Path(command[command.index('--build-path') + 1])
            self.assertEqual(build.parent, staged.parent)
            return subprocess.CompletedProcess(command, 1, 'synthetic compile diagnostic: ' + TOKEN + '\n')

        output, errors = io.StringIO(), io.StringIO()
        with patch.object(flash, 'ROOT', self.root), patch.object(flash, 'LIBRARIES', libraries), \
                patch.object(flash, 'verify_dependency'), patch.object(flash.subprocess, 'run', arduino_cli), \
                redirect_stdout(output), redirect_stderr(errors):
            status = flash.main(['--port', 'COM5', '--arduino-cli', cli_path, '--config-file', config_path])
        self.assertNotEqual(status, 0)
        self.assertEqual(len(staged_sketches), 1)
        self.assertFalse(staged_sketches[0].parent.exists())
        self.assertEqual(self.header.read_text(encoding='utf-8'), 'existing IDE header')
        self.assertIn('synthetic compile diagnostic', output.getvalue() + errors.getvalue())
        self.assertNotIn(TOKEN, output.getvalue() + errors.getvalue())


if __name__ == '__main__':
    unittest.main()
