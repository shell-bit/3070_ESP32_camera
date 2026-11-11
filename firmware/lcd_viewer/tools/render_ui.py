"""Render and test the production monitor UI with the installed LVGL 8.3.10."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


from project_layout import ROOT, BUILD, LVGL, verify_dependency, source_files, check_build_directory

OUT = BUILD / "ui-native"
CONFIG = LVGL / "src/lv_conf.h"


def run(command, *, cwd=ROOT, quiet=False):
    result = subprocess.run(
        [str(arg) for arg in command],
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode:
        print(subprocess.list2cmdline([str(arg) for arg in command]), file=sys.stderr)
        print(result.stdout, file=sys.stderr, end="")
        raise SystemExit(result.returncode)
    if result.stdout and not quiet:
        print(result.stdout, end="")
    return result.stdout.strip()


def hash_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def lvgl_library(tools, versions, jobs):
    cache = OUT / "lvgl"
    cache.mkdir(exist_ok=True)
    library = cache / "liblvgl.a"
    stamp = cache / "sha256"
    sources = sorted(
        path for path in (LVGL / "src").rglob("*.c")
        if path.relative_to(LVGL / "src").parts[0] not in {"demos", "examples"}
    )
    flags = ["-std=c99", "-O2", "-DLV_CONF_INCLUDE_SIMPLE", "-I", LVGL, "-I", CONFIG.parent]
    fingerprint = hashlib.sha256(str((tools, versions, flags)).encode())
    for path in sorted(sources + list(LVGL.rglob("*.h"))):
        fingerprint.update(path.relative_to(LVGL).as_posix().encode())
        fingerprint.update(path.read_bytes())
    digest = fingerprint.hexdigest()
    if library.is_file() and stamp.is_file() and stamp.read_text().strip() == digest:
        print("Reusing native LVGL 8.3.10 library")
        return library, digest

    print(f"Compiling LVGL 8.3.10 from {len(sources)} C files", flush=True)

    def compile_source(source):
        target = source.relative_to(LVGL).with_suffix(".o")
        (cache / target).parent.mkdir(parents=True, exist_ok=True)
        run([tools["gcc"], *flags, "-c", source, "-o", target], cwd=cache)
        return target

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        objects = list(pool.map(compile_source, sources))

    # A fresh archive prevents stale objects if the source set changes.
    pending = cache / "liblvgl-new.a"
    pending.unlink(missing_ok=True)
    response = cache / "objects.txt"
    response.write_text("\n".join(path.as_posix() for path in objects), encoding="ascii")
    run([tools["ar"], "rcs", pending, f"@{response.name}"], cwd=cache)
    pending.replace(library)
    stamp.write_text(digest + "\n", encoding="ascii")
    return library, digest


def compile_test(tools, library, name, sources, *, includes=(), flags=()):
    target = OUT / (name + (".exe" if os.name == "nt" else ""))
    run([
        tools["g++"], "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
        "-DLV_CONF_INCLUDE_SIMPLE", "-I", LVGL, "-I", CONFIG.parent,
        *[arg for path in includes for arg in ("-I", path)], *flags,
        *sources, library, "-o", target,
    ])
    return target


def test(command, log):
    with log.open("w", encoding="utf-8") as output:
        result = subprocess.run(
            [str(arg) for arg in command], cwd=ROOT, stdout=output, stderr=subprocess.STDOUT,
        )
    print(log.read_text(encoding="utf-8"), end="")
    if result.returncode:
        raise SystemExit(result.returncode)


def convert_screenshots(raw_path, png_path):
    from PIL import Image

    files = []
    for raw in sorted(raw_path.glob("*.rgb565")):
        source = raw.read_bytes()
        if len(source) != 320 * 240 * 2:
            raise SystemExit(f"Invalid RGB565 frame size in {raw}")
        rgb = bytearray()
        for low, high in zip(source[::2], source[1::2]):
            pixel = low | high << 8
            red, green, blue = pixel >> 11, (pixel >> 5) & 63, pixel & 31
            rgb.extend((red << 3 | red >> 2, green << 2 | green >> 4, blue << 3 | blue >> 2))
        png = png_path / raw.with_suffix(".png").name
        Image.frombytes("RGB", (320, 240), bytes(rgb)).save(png)
        files.append({"file": png.relative_to(OUT).as_posix(), "sha256": hash_file(png)})
    return files


def main():
    check_build_directory()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--lvgl-only", action="store_true", help="Build the native LVGL library cache")
    group.add_argument("--baseline", type=Path, help="Run the focused controller check on a baseline source directory")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if not CONFIG.is_file() or "version=8.3.10" not in (LVGL / "library.properties").read_text():
        parser.error("The local Waveshare LVGL 8.3.10 dependency is missing")
    verify_dependency()

    tools = {name: shutil.which(name) for name in ("gcc", "g++", "ar")}
    if any(path is None for path in tools.values()):
        parser.error("gcc, g++ and ar must be on PATH")
    versions = {name: run([tool, "--version"], quiet=True).splitlines()[0] for name, tool in tools.items()}
    OUT.mkdir(parents=True, exist_ok=True)
    library, digest = lvgl_library(tools, versions, args.jobs)
    if args.lvgl_only:
        return

    test_root = ROOT / "tests"
    fakes = test_root / "fakes"
    if args.baseline:
        source = args.baseline.resolve()
        baseline = compile_test(
            tools, library, "test_preview_ui_baseline",
            [test_root / "test_preview_ui.cpp", source / "preview_ui.cpp"],
            includes=(fakes, source), flags=("-Wno-misleading-indentation",),
        )
        test([baseline, "unconfigured"], OUT / "baseline-test-results.log")
        return

    manifest = OUT / "manifest.json"
    manifest.unlink(missing_ok=True)
    font_source = ROOT / "src/ui/monitor_font_28.c"
    font_object = OUT / "monitor_font_28.o"
    run([
        tools["gcc"], "-std=c99", "-O2", "-DLV_CONF_INCLUDE_SIMPLE",
        "-I", LVGL, "-I", CONFIG.parent, "-c", font_source, "-o", font_object,
    ])
    view = [ROOT / "src/ui/monitor_view.cpp", font_object]
    renderer = compile_test(tools, library, "render_ui", [test_root / "render_ui.cpp", *view], includes=(ROOT,))
    controller = compile_test(
        tools, library, "test_preview_ui",
        [test_root / "test_preview_ui.cpp", ROOT / "src/ui/preview_ui.cpp", *view],
        includes=(fakes, ROOT),
    )
    protocol = compile_test(
        tools, library, "test_mjpeg_parser",
        [test_root / "test_mjpeg_parser.cpp", ROOT / "src/preview/mjpeg_parser.cpp"],
    )
    raw_path = OUT / "frames"
    png_path = OUT / "png"
    for path, suffix in ((raw_path, "*.rgb565"), (png_path, "*.png")):
        path.mkdir(exist_ok=True)
        for stale in path.glob(suffix):
            stale.unlink()
    test([renderer, str(raw_path.resolve())], OUT / "view-test-results.log")
    test([controller], OUT / "controller-test-results.log")
    test([controller, "unconfigured"], OUT / "controller-unconfigured-results.log")
    test([protocol], OUT / "protocol-test-results.log")
    screenshots = convert_screenshots(raw_path, png_path)
    manifest.write_text(json.dumps({
        "description": "Host LVGL render with a synthetic RGB565 camera frame",
        "toolchain": versions,
        "python": sys.version.split()[0],
        "lvgl_version": "8.3.10",
        "lvgl_config_sha256": hash_file(CONFIG),
        "lvgl_library_cache_key": digest,
        "source_sha256": {
            path.relative_to(ROOT).as_posix(): hash_file(path)
            for path in (
                *source_files().values(), test_root / "render_ui.cpp", test_root / "test_mjpeg_parser.cpp",
                test_root / "test_preview_ui.cpp", fakes / "native_display.h", fakes / "Arduino.h",
                Path(__file__).resolve(), ROOT / 'tools/project_layout.py', ROOT / 'dependencies.lock.json',
            )
        },
        "screenshots": screenshots,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(screenshots)} PNGs and test logs to {OUT}")


if __name__ == "__main__":
    main()
