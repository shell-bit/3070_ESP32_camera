"""Compile and run production configuration tests with synthetic inputs only.

This command compiles host C/C++ code. It does not compile firmware, touch a
device, or read setup.json/camera_setup.json. Evidence is separate from the
retained MJPEG host-test manifest. The MD5 fake checks content identity only.
"""

import json
import os
from pathlib import Path
import shutil
import subprocess

from project_layout import BUILD, PROJECT_ROOT, ROOT, check_build_directory, sha


def main():
    check_build_directory()
    output = BUILD / 'config-tests'
    output.mkdir(parents=True, exist_ok=True)
    manifest = output / 'config-test-manifest.json'
    manifest.unlink(missing_ok=True)

    cjson = PROJECT_ROOT / 'firmware/camera/vendor/cjson'
    test = ROOT / 'tests/test_built_setup.cpp'
    production = ROOT / 'src/config/viewer_config.cpp'
    fakes = ROOT / 'tests/fakes/config'
    inputs = sorted([
        test, production, production.with_suffix('.h'),
        cjson / 'cJSON.c', cjson / 'cJSON.h',
        *(fakes / name for name in ('Arduino.h', 'Preferences.h', 'MD5Builder.h', 'nvs.h')),
        Path(__file__).resolve(), ROOT / 'tools/project_layout.py',
    ])
    missing = [path.relative_to(PROJECT_ROOT).as_posix() for path in inputs if not path.is_file()]
    if missing:
        raise SystemExit('Missing configuration-test dependency: ' + ', '.join(missing) +
                         '. Real vendored camera cJSON sources are required for this host-only suite.')
    compilers = {language: shutil.which(name) for language, name in (('c', 'gcc'), ('c++', 'g++'))}
    if not all(compilers.values()):
        raise SystemExit('Configuration host tests require gcc and g++ on PATH; no compiler was run.')

    def hashes():
        return {path.relative_to(PROJECT_ROOT).as_posix(): sha(path) for path in inputs}

    before = hashes()
    log_path = output / 'config-test-results.log'
    target = output / ('test_built_setup.exe' if os.name == 'nt' else 'test_built_setup')
    cjson_object = output / 'cJSON.o'
    with log_path.open('w', encoding='utf-8') as log:
        def run(command):
            arguments = [str(argument) for argument in command]
            log.write('$ ' + subprocess.list2cmdline(arguments) + '\n')
            log.flush()
            result = subprocess.run(arguments, cwd=PROJECT_ROOT, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                                    errors='replace', timeout=120)
            if result.stdout:
                print(result.stdout, end='', flush=True)
                log.write(result.stdout)
            log.write(f'Exit: {result.returncode}\n')
            log.flush()
            if result.returncode:
                raise SystemExit(result.returncode)
            return result.stdout

        versions = {language: run([compiler, '--version']).splitlines()[0]
                    for language, compiler in compilers.items()}
        # Keep the vendor parser as C; production code and Arduino fakes use C++17.
        run([compilers['c'], '-std=c11', '-O2', '-Wall', '-Wextra',
             '-c', cjson / 'cJSON.c', '-o', cjson_object])
        run([compilers['c++'], '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
             '-I', fakes, '-I', cjson, test, production, cjson_object, '-o', target])
        cases = run([target, '--list']).splitlines()
        if not cases or len(cases) != len(set(cases)):
            raise SystemExit('Configuration test binary returned an empty or duplicate case inventory')
        # A process per case resets private production globals without test APIs.
        for case in cases:
            run([target, case])
        summary = f'PASS: {len(cases)} production configuration cases'
        print(summary, flush=True)
        log.write(summary + '\n')
    if before != hashes():
        raise SystemExit('Configuration test inputs changed during verification; rerun tests')
    manifest.write_text(json.dumps({
        'suite': 'lcd-configuration',
        'compilers': versions,
        'cases': cases,
        'source_sha256': before,
        'log': log_path.relative_to(PROJECT_ROOT).as_posix(),
        'log_sha256': sha(log_path),
        'artifact_sha256': {path.relative_to(PROJECT_ROOT).as_posix(): sha(path)
                            for path in (target, cjson_object)},
        'limitations': 'Synthetic NVS/Serial/ESP and content-identity digest; no hardware or MD5 proof.',
    }, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
