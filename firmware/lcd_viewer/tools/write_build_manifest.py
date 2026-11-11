"""Record the completed Arduino build, refusing stale staged source or absent images."""
from datetime import datetime, timezone
import json
import re
from project_layout import ROOT, BUILD, sha, source_hashes


def main():
    sources = source_hashes()
    if source_hashes(BUILD / 'sketch/lcd_viewer') != sources:
        raise SystemExit('Build staging differs from current source')
    settings = json.loads((BUILD / 'build-settings.json').read_text(encoding='utf-8-sig'))

    log = (BUILD / 'compile.log').read_text(encoding='utf-8-sig')
    program = re.search(r'Sketch uses (\d+) bytes .*?Maximum is (\d+) bytes', log)
    memory = re.search(r'Global variables use (\d+) bytes .*?Maximum is (\d+) bytes', log)
    if not program or not memory:
        raise SystemExit('Successful Arduino memory summary missing from compile.log')

    artifacts = {}
    for name in ('boot_app0.bin', 'lcd_viewer.ino.bin', 'lcd_viewer.ino.bootloader.bin',
                 'lcd_viewer.ino.partitions.bin', 'lcd_viewer.ino.merged.bin'):
        path = BUILD / 'arduino' / name
        if not path.is_file() or not path.stat().st_size:
            raise SystemExit('Missing build artifact: ' + name)
        artifacts[name] = {'bytes': path.stat().st_size, 'sha256': sha(path)}
    result = {
        'generated_at_utc': datetime.now(timezone.utc).isoformat(),
        'board': 'Waveshare ESP32-S3-Touch-LCD-2.8 V2',
        'arduino_esp32': '3.3.12', 'esp_idf': '5.5.5', 'lvgl': '8.3.10',
        'fqbn': settings['fqbn'], 'dependencies': {'lvgl': settings['lvgl']},
        'sources': sources, 'artifacts': artifacts, 'hardware_tested': False,
        'verification': {
            'host_tests': 'passed by build.ps1 before compilation',
            'firmware_build': 'passed; build.ps1 invokes this only after compile exit code 0',
            'program_bytes': int(program[1]), 'program_capacity': int(program[2]),
            'static_ram_bytes': int(memory[1]), 'static_ram_capacity': int(memory[2]),
            'compile_log_sha256': sha(BUILD / 'compile.log'),
            'staged_sources_equal_current_sources': True,
        },
    }
    (BUILD / 'build-manifest.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'verified_sources': len(sources), 'program_bytes': int(program[1]),
                      'static_ram_bytes': int(memory[1]), 'artifacts': len(artifacts)}))


if __name__ == '__main__':
    main()
