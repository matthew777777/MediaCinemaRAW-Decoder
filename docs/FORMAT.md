# Type 7 frame payload format

All integers are little-endian. This documents the payload bytes consumed by
`mediacinemaraw::decode()`. The container that carries these payloads is
described in [CONTAINER.md](CONTAINER.md).

## Layout overview

```
u32 padded_width   # (width + 63) / 64 * 64
u32 height
u32 bits_offset    # file offset of the bits stream
u32 refs_offset    # file offset of the refs stream
bytes tile_data    # [16, bits_offset)
bytes bits_stream  # [bits_offset, refs_offset)
bytes refs_stream  # [refs_offset, end)
```

`width` is even and `height` is a multiple of four. Columns at `x >= width`
are padding: the decoder discards them.

## Tiles

The frame is covered row-major by 64x4 tiles (`padded_width / 64` by
`height / 4`). Each tile holds four channels, one per Bayer phase: channel
`c` contains the 64 samples with `x % 2 == c % 2` and `y % 2 == c / 2`
relative to the tile origin, ordered by `(row_pair * 32 + column_pair)`.

Per channel the payload records a bit width and a reference (`lo`, the
channel minimum) and packs the 64 `sample - lo` deltas:

| max delta   | width | packed size | packing                        |
| ----------- | ----- | ----------- | ------------------------------ |
| 0           | 0     | 0           | nothing stored                 |
| < 2,4,16    | 1,2,4 | w*8         | generic lane packing (below)   |
| < 8         | 3     | 24          | 3 rows of 8, see code          |
| < 32        | 5     | 40          | 5 rows of 8, see code          |
| < 64        | 6     | 48          | 6 rows of 8, see code          |
| < 256       | 8     | 64          | low bytes                      |
| < 1024      | 10    | 80          | per 32-sample half: 32 low bytes + 8 high bytes (2 bits x 4 groups per lane) |
| otherwise   | 16    | 128         | raw u16 literals               |

Deltas are packed into eight interleaved lanes: sample `i` belongs to lane
`i % 8`. Generic widths (1, 2, 4) store `8 / width` samples per byte,
lane-major. The 3/5/6-bit lane layouts are fixed bit shuffles; the exact
mapping is the `UnpackChannelDeltas()` implementation in
`src/detail/BitUnpacking.h`, pinned by the golden payload in
`tests/DecoderTests.cpp`.

Any other width, a delta outside its width, or `lo + delta > 0xFFFF` is a
corrupt payload; the tile section must end exactly at `bits_offset`.

## Bits and refs streams

Both streams share one structure. Each starts with a `u32` count equal to
the channel count rounded up to a multiple of 64, followed by groups of 64
values. Every group is a 2-byte header plus packed deltas:

```
u8  (width_nibble << 4) | (ref >> 8)
u8  ref & 0xFF
bytes packed (value - ref) deltas at width_nibble
```

`ref` is at most 4095; the 16-bit literal mode is spelled `15` in the
four-bit width field. Values past the true channel count are zero padding,
and the decoder rejects non-zero tails. The width stream must occupy
`[bits_offset, refs_offset)` exactly; the reference stream runs from
`refs_offset` to the end of the payload.

## Reader conventions

- The caller supplies the visible dimensions (from the frame JSON); the
  header's padded width and height must agree with them.
- Only compression type 7 is accepted; legacy type 6 is rejected with an
  explicit error.
- Decoding yields row-major `uint16` Bayer samples with no demosaic,
  black-level subtraction, scaling, or orientation applied.
