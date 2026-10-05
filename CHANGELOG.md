# Changelog

## Unreleased

- Restructured the codebase without changing the wire format: decoder
  internals moved to documented `src/detail/` modules (SIMD, delta
  unpacking, tile loop, container constants), public headers documented,
  validation suite split into named cases. Decoding is bit-identical; the
  240-combination interop matrix and the new golden payload pin it.
- Added `docs/FORMAT.md` and `docs/CONTAINER.md` wire-format notes.
- CMake: warning flags, installable package config, position-independent
  code, and an optional `MEDIACINEMARAW_ENCODER_DIR` interop test target.
  The forced `-O3` is gone; standard build types apply. Tests now compile
  with `-UNDEBUG` so the assert-based checks stay live in Release builds.
- Added an x86 SSE2 fast path alongside the ARM NEON unpacking paths.
- Interop script accepts `--cxx` and discovers decoder/encoder sources
  automatically. `mcraw_dump` gained `--help` and stricter arguments.
- CI now covers Debug builds and the sibling-encoder interop suite.

## 0.1.0

- Lossless type-7 frame decoder with padded-width handling and Bayer
  reinterleaving, NEON fast paths, and a portable scalar fallback.
- Version-3 container reader: frames, JSON metadata, PCM16 audio, and
  gyro/accelerometer motion, with OIS and unknown item skipping.
