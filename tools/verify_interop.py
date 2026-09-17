#!/usr/bin/env python3
"""Build and run decoder interop tests against an encoder checkout.

The encoder sources stay outside this repo; they are only compiled
together for the test binary, mirroring the encoder-side oracle setup.
"""
import pathlib
import subprocess
import sys
import tempfile

if len(sys.argv) != 2:
    raise SystemExit("usage: verify_interop.py /path/to/MediaCinemaRAW-Encoder")

root = pathlib.Path(__file__).resolve().parents[1]
encoder = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="mediacinemaraw-decode-") as tmp:
    build = pathlib.Path(tmp)
    flags = [
        "clang++", "-std=c++17", "-O2", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-I" + str(root / "include"),
        "-I" + str(encoder / "include"),
    ]
    sources = [
        root / "src/Decoder.cpp",
        root / "src/ContainerReader.cpp",
        encoder / "src/Encoder.cpp",
        encoder / "src/ContainerWriter.cpp",
        root / "tests/InteropTests.cpp",
    ]
    for source in sources:
        if not source.exists():
            raise SystemExit(f"missing source: {source}")
    objects = []
    for source in sources:
        obj = build / (source.stem + ".o")
        subprocess.run(flags + ["-c", str(source), "-o", str(obj)], check=True)
        objects.append(str(obj))
    executable = build / "interop-tests"
    subprocess.run(flags + objects + ["-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, cwd=str(build))
