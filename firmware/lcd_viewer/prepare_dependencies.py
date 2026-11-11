"""Verify or install the pinned project-local LVGL used by IDE, CLI and native tests."""
from pathlib import Path
import json
import shutil
import sys
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'tools'))
from project_layout import BUILD, LIBRARIES, LVGL, sha, verify_dependency, reject_link, check_build_directory

LOCK = json.loads((ROOT / 'dependencies.lock.json').read_text(encoding='utf-8'))['lvgl']
ARCHIVE = BUILD / 'downloads/waveshare-v2-demo.zip'


def main():
    if LVGL.exists():
        verify_dependency()
        print('Pinned LVGL 8.3.10 already verified:', LVGL)
        return
    check_build_directory()
    # Never replace a library that the user's IDE may be using.
    LIBRARIES.mkdir(parents=True, exist_ok=True)
    reject_link(LIBRARIES)
    ARCHIVE.parent.mkdir(parents=True, exist_ok=True)
    if not ARCHIVE.exists():
        urllib.request.urlretrieve(LOCK['url'], ARCHIVE)
    if sha(ARCHIVE) != LOCK['archive_sha256']:
        raise SystemExit("Official archive changed: hash mismatch; review the new V2 source before updating the pin")
    with tempfile.TemporaryDirectory(prefix='lvgl-', dir=BUILD) as temporary:
        pending = Path(temporary) / 'lvgl'
        pending.mkdir()
        with zipfile.ZipFile(ARCHIVE) as archive:
            for entry in archive.infolist():
                if not entry.filename.startswith(LOCK['archive_prefix']) or entry.is_dir():
                    continue
                relative = entry.filename[len(LOCK['archive_prefix']):]
                target = (pending / relative).resolve()
                if not target.is_relative_to(pending.resolve()):
                    raise SystemExit('Unsafe archive path')
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(entry) as source, target.open('wb') as output:
                    shutil.copyfileobj(source, output)
        verify_dependency(pending)
        if LVGL.exists():
            raise SystemExit('Library appeared during setup; refusing overwrite')
        pending.rename(LVGL)
    print('LVGL 8.3.10 ready:', LIBRARIES)


if __name__ == "__main__":
    main()
