"""Compile and run the LCD's portable production-code tests without a board."""

import json
import os
from pathlib import Path
import shutil
import subprocess

from project_layout import ROOT, BUILD, check_build_directory, sha


def main():
    check_build_directory()
    BUILD.mkdir(parents=True, exist_ok=True)
    manifest = BUILD / 'host-test-manifest.json'
    manifest.unlink(missing_ok=True)
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('g++ must be on PATH')
    test_root = ROOT / 'tests'
    suites = {
        'test_mjpeg_parser': [test_root / 'test_mjpeg_parser.cpp', ROOT / 'src/preview/mjpeg_parser.cpp'],
    }
    inputs = {*[p for paths in suites.values() for p in paths], Path(__file__).resolve(),
              ROOT / 'tools/project_layout.py',
              *[ROOT / 'src/preview' / name for name in
                ('mjpeg_parser.h', 'session_clock.h', 'session_state.h', 'frame_deadline.h')]}
    inputs = sorted(inputs)
    before = {p.relative_to(ROOT).as_posix(): sha(p) for p in inputs}
    log_path = BUILD / 'test-results.log'
    with log_path.open('w', encoding='utf-8') as log:
        def run(command):
            result = subprocess.run([str(arg) for arg in command], cwd=ROOT,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, encoding='utf-8', errors='replace', timeout=120)
            if result.stdout:
                print(result.stdout, end='', flush=True)
                log.write(result.stdout)
                log.flush()
            if result.returncode:
                raise SystemExit(result.returncode)
            return result.stdout

        version = run([compiler, '--version']).splitlines()[0]
        for name, sources in suites.items():
            print('Running ' + name, flush=True)
            log.write('Running ' + name + '\n')
            target = BUILD / (name + ('.exe' if os.name == 'nt' else ''))
            run([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                 *sources, '-o', target])
            run([target])
        summary = f'PASS: {len(suites)} portable host suite(s)'
        log.write(summary + '\n')
        print(summary, flush=True)
    after = {p.relative_to(ROOT).as_posix(): sha(p) for p in inputs}
    if before != after:
        raise SystemExit('Host test inputs changed during verification; rerun tests')
    manifest.write_text(json.dumps({'compiler': version, 'suites': list(suites),
                                    'source_sha256': before, 'log_sha256': sha(log_path)},
                                   indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
