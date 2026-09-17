# MediaCinemaRAW Decoder

A portable C++17 decoder for the **MediaCinemaRAW** lossless RAW-frame format.
It decodes compression type 7 frame payloads and reads version-3 `.mcraw`
containers (frames, PCM16 audio, gyro, accelerometer).

This is an independent decoder implementation. During development, the separate
[`motioncam-decoder`](https://github.com/mirsadm/motioncam-decoder) project was
consulted as a reference for understanding the `.mcraw` container format,
metadata structures, and expected decoder behavior, and the sibling
`MediaCinemaRAW-Encoder` checkout is also used for interoperability testing.

The decoder code in this repository was written independently and does not
include or link against `motioncam-decoder`. No decoder source code was
intentionally copied. Because the reference implementation was inspected
during development, however, this project does **not** claim to be a formal
clean-room implementation.

The sibling encoder project discloses the same relationship: it is an
independent decoder-compatible encoder implementation developed with the
reference consulted for format, metadata, and behavior understanding plus
interoperability testing, written independently with no include/link against
`motioncam-decoder` and no intentional copying, likewise not claiming formal
clean-room status. See [FAQ](FAQ.md).

## Features

- Lossless type-7 frame decoding (`1/2/3/4/5/6/8/10/16`-bit blocks + trivial blocks)
- Padded-width handling (`(width+63)/64*64`) and Bayer reinterleaving
- Version-3 container reader with frame/audio/gyro/accelerometer indexes
- OIS and unknown item skipping, strict offset/size validation
- ARM NEON fast paths with a portable scalar fallback
- No runtime dependencies beyond the C++ standard library (JSON is returned as raw strings)

Legacy compression type 6 is rejected with a clear error; only type 7 is supported,
matching the encoder.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

To run the full interoperability suite against a separate encoder checkout:

```sh
python3 tools/verify_interop.py /path/to/MediaCinemaRAW-Encoder
```

The `mcraw_dump` helper lists container contents and can export the first
frame to PGM and the audio to WAV:

```sh
./build/mcraw_dump input.mcraw --pgm frame.pgm --wav audio.wav
```

## API

```cpp
#include <MediaCinemaRAW/Decoder.h>
#include <MediaCinemaRAW/ContainerReader.h>

std::vector<uint16_t> pixels;
mediacinemaraw::decode(payload.data(), payload.size(), width, height, pixels);

mediacinemaraw::ContainerReader reader("clip.mcraw");
for (auto ts : reader.frameTimestamps()) {
    mediacinemaraw::Frame frame;
    reader.loadFrame(ts, frame);
}
std::vector<mediacinemaraw::AudioChunk> audio;
reader.loadAudio(audio);
if (reader.hasGyroData()) {
    std::vector<mediacinemaraw::MotionSample> gyro;
    reader.loadGyroData(gyro);
}
if (reader.hasAccelerometerData()) {
    std::vector<mediacinemaraw::MotionSample> accel;
    reader.loadAccelerometerData(accel);
}
```

Frame and container metadata are returned as JSON strings. Parse `width`,
`height`, and `compressionType` from the frame JSON; container audio rate and
channels are available via `audioSampleRateHz()` / `numAudioChannels()`.

## Format compatibility

Interop covers 240 deterministic combinations of RAW16, RAW10, crop,
downscale, stride, constant blocks, and supported bit widths. Each payload is
encoded with the reference encoder and decoded here pixel-for-pixel, plus a
container round-trip (frames, audio, gyro) through the encoder writer.

## FAQ

See [FAQ](FAQ.md) for the shared whole-project FAQ covering the encoder,
decoder, format, troubleshooting, and DNG export notes.

## License

GPL-3.0-only. See [LICENSE](LICENSE).
