"""Shared source, build and dependency boundaries for the LCD viewer tools."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import stat

ROOT = Path(__file__).resolve().parents[1]
PROJECT_ROOT = ROOT.parents[1]
BUILD = PROJECT_ROOT / 'build/lcd_viewer'
LIBRARIES = ROOT.parent / 'libraries'
LVGL = LIBRARIES / 'lvgl'
SOURCE_SUFFIXES = {'.ino', '.cpp', '.c', '.h', '.hpp'}
SOURCE_DOCUMENTS = {'src/ui/design.md'}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def reject_link(path):
    info = path.lstat()
    if path.is_symlink() or getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
        raise ValueError(f'Linked source/dependency is not allowed: {path}')


def regular_files(root):
    """Walk without following junctions or symlinks, including on Windows."""
    reject_link(root)
    for path in sorted(root.iterdir()):
        reject_link(path)
        if path.is_dir():
            yield from regular_files(path)
        elif path.is_file():
            yield path


def check_build_directory():
    """Reject output junctions before any compiler, renderer or downloader writes."""
    for path in (PROJECT_ROOT / 'build', BUILD):
        if path.exists() or path.is_symlink():
            reject_link(path)
    if BUILD.exists():
        for _ in regular_files(BUILD):
            pass


def source_files(root=ROOT):
    root = Path(root)
    reject_link(root)
    candidates = [p for p in root.iterdir() if p.suffix.lower() in SOURCE_SUFFIXES]
    if (root / 'src').exists():
        candidates.extend(regular_files(root / 'src'))
    result = {}
    for path in sorted(candidates):
        reject_link(path)
        name = path.relative_to(root).as_posix()
        if any(not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]{0,62}', part)
               or part.endswith('.') for part in Path(name).parts):
            raise ValueError(f'Invalid Arduino source name: {name}')
        if path.is_file() and name in SOURCE_DOCUMENTS:
            continue
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            raise ValueError(f'Unexpected file in Arduino sources: {path}')
        if path.suffix == '.ino' and name != 'lcd_viewer.ino':
            raise ValueError(f'Only the root lcd_viewer.ino sketch entry is supported: {name}')
        result[name] = path
    if 'lcd_viewer.ino' not in result:
        raise ValueError('Missing lcd_viewer.ino entry point')
    return result


def source_hashes(root=ROOT):
    return {name: sha(path) for name, path in source_files(root).items()}


def dependency_fingerprint(library=LVGL):
    library = Path(library)
    digest = hashlib.sha256()
    files = list(regular_files(library))
    # Sort by complete relative path, independent of the recursive traversal order.
    for path in sorted(files, key=lambda item: item.relative_to(library).as_posix()):
        digest.update(path.relative_to(library).as_posix().encode())
        digest.update(b'\0')
        digest.update(bytes.fromhex(sha(path)))
    return {'files': len(files), 'tree_sha256': digest.hexdigest(),
            'config_sha256': sha(library / 'src/lv_conf.h')}


def verify_dependency(library=LVGL):
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text(encoding='utf-8'))
    actual = dependency_fingerprint(library)
    if actual != lock['lvgl']['fingerprint']:
        raise ValueError('LVGL differs from the pinned Waveshare 8.3.10 dependency; refusing build')
    return actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('paths', 'sources', 'verify-dependency'))
    parser.add_argument('--library', type=Path, default=LVGL)
    args = parser.parse_args()
    if args.command == 'paths':
        check_build_directory()
        result = {'root': str(ROOT), 'project_root': str(PROJECT_ROOT), 'build': str(BUILD),
                  'libraries': str(LIBRARIES), 'lvgl': str(LVGL)}
    elif args.command == 'sources':
        result = source_hashes()
    else:
        result = verify_dependency(args.library)
    print(json.dumps(result))


if __name__ == '__main__':
    main()
