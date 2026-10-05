#!/usr/bin/env python3
"""Decode 240 generated payloads here that the sibling encoder produced.

Builds tests/InteropTests.cpp together with this repo's decoder sources and
the encoder sources from a separate checkout, under AddressSanitizer/UBSan,
and runs the round-trip suite. Encoder sources are compiled in place, never
vendored.

Usage:
    python3 tools/verify_interop.py /path/to/MediaCinemaRAW-Encoder
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile


def build_and_run(root: pathlib.Path, encoder: pathlib.Path, cxx: str) -> None:
    decoder_sources = sorted(root.glob("src/*.cpp"))
    if not decoder_sources:
        raise SystemExit(f"no decoder sources under {root / 'src'}")
    encoder_sources = sorted(encoder.glob("src/*.cpp"))
    if not encoder_sources:
        raise SystemExit(f"no encoder sources under {encoder / 'src'}")
    driver = root / "tests/InteropTests.cpp"
    if not driver.exists():
        raise SystemExit(f"missing source: {driver}")

    with tempfile.TemporaryDirectory(prefix="mediacinemaraw-decode-") as tmp:
        build = pathlib.Path(tmp)
        flags = [
            cxx, "-std=c++17", "-O2", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-I" + str(root / "include"),
            "-I" + str(encoder / "include"),
        ]
        objects = []
        for source in decoder_sources + encoder_sources + [driver]:
            obj = build / (source.stem + ".o")
            subprocess.run(flags + ["-c", str(source), "-o", str(obj)], check=True)
            objects.append(str(obj))
        executable = build / "interop-tests"
        subprocess.run(flags + objects + ["-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, cwd=str(build))


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Round-trip 240 payloads through the sibling encoder.")
    parser.add_argument("encoder_dir",
                        help="path to a MediaCinemaRAW-Encoder checkout")
    parser.add_argument("--cxx", default="clang++",
                        help="compiler to use (default: clang++)")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    build_and_run(root, pathlib.Path(args.encoder_dir).resolve(), args.cxx)


if __name__ == "__main__":
    sys.exit(main())
