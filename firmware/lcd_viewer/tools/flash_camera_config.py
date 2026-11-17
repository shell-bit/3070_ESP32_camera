"""Prepare camera settings for the LCD Viewer; optionally upload over USB."""

import argparse
from ipaddress import IPv4Address
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile

from project_layout import LIBRARIES, ROOT, source_files, verify_dependency


CORE_VERSION = '3.3.12'
FQBN = ('esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,'
        'PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,EraseFlash=none')
SETUP_FILE = 'camera_setup.json'
HEADER_FILE = 'camera_setup.local.h'
HOST_LABEL = re.compile(r'[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?')
TOKEN = re.compile(r'[a-zA-Z0-9._~+/=-]{24,128}')


def unique_object(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise ValueError(f'{SETUP_FILE} contains a duplicate key.')
        obj[key] = value
    return obj


def valid_host(host):
    if not isinstance(host, str) or not 1 <= len(host) <= 127:
        return False
    if re.fullmatch(r'[0-9.]+', host):
        try:
            IPv4Address(host)
        except ValueError:
            return False
        return True
    return all(HOST_LABEL.fullmatch(label) for label in host.split('.'))


def read_setup(path):
    try:
        text = path.read_text(encoding='utf-8-sig')
    except FileNotFoundError:
        raise ValueError(f'Copy camera_setup.example.json to {SETUP_FILE} and fill in both fields.') from None
    except (OSError, UnicodeError):
        raise ValueError(f'Cannot read {SETUP_FILE} as UTF-8.') from None
    try:
        data = json.loads(text, object_pairs_hook=unique_object)
    except (json.JSONDecodeError, RecursionError):
        raise ValueError(f'{SETUP_FILE} must contain valid JSON.') from None
    if not isinstance(data, dict) or data.keys() != {'camera_host', 'token'}:
        raise ValueError(f'{SETUP_FILE} must contain exactly camera_host and token.')
    host, token = data['camera_host'], data['token']
    if not valid_host(host):
        raise ValueError(f'{SETUP_FILE}: camera_host must be a hostname or IPv4 address (up to 127 characters).')
    if not isinstance(token, str) or not TOKEN.fullmatch(token):
        raise ValueError(f'{SETUP_FILE}: token must be 24–128 characters (letters, digits or -._~+/=).')
    return {'camera_host': host, 'token': token}


def write_header(settings, path):
    payload = json.dumps(settings, separators=(',', ':'))
    header = ('#pragma once\n'
              '// Generated from camera_setup.json; contains a camera pairing token.\n'
              'namespace lcd {\n'
              f'inline constexpr char cameraSetupJson[] = R"camera({payload})camera";\n'
              f'inline constexpr char cameraSetupRevision[] = "{secrets.token_hex(16)}";\n'
              '}\n')
    try:
        # mkstemp creates an owner-only file on POSIX. Keep the temp file on the same
        # volume so a failed write cannot replace the previous generated header.
        fd, name = tempfile.mkstemp(prefix='.camera_setup.', suffix='.tmp', dir=path.parent)
        pending = Path(name)
        try:
            with os.fdopen(fd, 'w', encoding='ascii', newline='\n') as output:
                if os.name == 'posix':
                    os.fchmod(output.fileno(), 0o600)
                output.write(header)
            pending.replace(path)
        finally:
            pending.unlink(missing_ok=True)
    except OSError:
        raise ValueError(f'Cannot write {HEADER_FILE}. Check that the sketch directory is writable.') from None


def arduino_cli(command, **kwargs):
    try:
        return subprocess.run(command, stdout=subprocess.PIPE, text=True, encoding='utf-8',
                              errors='replace', **kwargs)
    except OSError:
        raise ValueError('Cannot run Arduino CLI. Install it or pass its path with --arduino-cli.') from None


def check_core(command):
    result = arduino_cli(command + ['core', 'list', '--json'], stderr=subprocess.PIPE)
    if result.returncode:
        raise ValueError('Arduino CLI could not list installed cores. Check its installation and --config-file.')
    try:
        platforms = json.loads(result.stdout)['platforms']
        pinned = any(p['id'] == 'esp32:esp32' and p['installed_version'] == CORE_VERSION for p in platforms)
    except (ValueError, KeyError, TypeError):
        pinned = False
    if not pinned:
        raise ValueError(f'Install Arduino-ESP32 {CORE_VERSION} before uploading.')


def upload(settings, args):
    command = [args.arduino_cli]
    if args.config_file:
        command += ['--config-file', args.config_file]
    check_core(command)
    library = LIBRARIES / 'lvgl'
    try:
        verify_dependency(library)
    except (OSError, ValueError):
        raise ValueError('Pinned LVGL 8.3.10 is unavailable. Run prepare_dependencies.py before uploading.') from None

    with tempfile.TemporaryDirectory(prefix='lcd-viewer-flash-') as temporary:
        work = Path(temporary)
        sketch = work / 'lcd_viewer'
        for name, source in source_files(ROOT).items():
            if name == HEADER_FILE:
                continue
            destination = sketch / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
        write_header(settings, sketch / HEADER_FILE)
        print('Compiling and uploading LCD Viewer. This can take several minutes.', flush=True)
        result = arduino_cli(command + ['compile', '--fqbn', FQBN, '--upload',
                                        '--port', args.port, '--protocol', 'serial', '--library', str(library),
                                        '--build-path', str(work / 'build'), str(sketch)],
                             stderr=subprocess.STDOUT)
        if result.stdout:
            # Compiler diagnostics can quote lines from the generated header.
            diagnostics = result.stdout.replace(settings['token'], '[redacted]')
            print(diagnostics, end='', file=sys.stderr if result.returncode else sys.stdout)
        if result.returncode:
            raise ValueError('LCD Viewer compile or upload failed.')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', help='USB serial port; omit to generate the header for Arduino IDE')
    parser.add_argument('--arduino-cli', default='arduino-cli', help='path to Arduino CLI (default: arduino-cli on PATH)')
    parser.add_argument('--config-file', help='Arduino CLI YAML configuration, if needed')
    args = parser.parse_args(argv)
    try:
        settings = read_setup(ROOT / SETUP_FILE)
        if args.port:
            upload(settings, args)
        else:
            write_header(settings, ROOT / HEADER_FILE)
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 1
    except OSError:
        print('Cannot stage the LCD Viewer sketch or build files.', file=sys.stderr)
        return 1
    print('Uploaded LCD Viewer camera setup.' if args.port else f'Prepared {HEADER_FILE} for Arduino IDE upload.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
