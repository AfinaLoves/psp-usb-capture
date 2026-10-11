# Notes

Things I learned the hard way while getting this to work. Numbers are from a
PSP-3000 on PROMOD-C2p, mostly with Undertale (30 fps, 32-bit framebuffer)
and Project DIVA 2nd (3D, CPU bound).

## USB

- USB 2.0 high speed. A bulk IN endpoint tops out around **15 MiB/s**, but
  only if the sending thread runs at priority `0x10`. At the default `0x20`
  the game keeps preempting it and throughput drops to about a third.
- `sceUsbbdReqSend` refuses VRAM addresses (`0x80243002`, all four aliases),
  so every frame is first copied into a RAM buffer in the plugin.
- Queue depth (1 vs 2 vs 4 requests in flight) hardly matters at `0x10`.

## Video

- A 32-bit frame (480×272) is 510 KiB, ~34 ms on the wire. That caps 8888 at
  ~29 fps; 16-bit (565) halves it to ~18 ms.
- Reading VRAM through the **uncached** alias cost 17 ms of CPU per frame and
  slowed Undertale from 30 to 22–29 fps. Reading through the **cached** alias
  after `sceKernelDcacheWritebackInvalidateAll()` takes 4–5 ms; Undertale
  stays at 29.9 fps while streaming.
- Running the copy below the game's priority doesn't work: the stream starves
  (11 of 30 fps arrive) and the game isn't any faster. Worse, a slow copy
  tears the frame because the game starts drawing into the same buffer.
- 3D games that already use all the CPU (DIVA: 27 fps without the stream) lose
  5–8 fps from USB traffic at `0x10` alone, even without copying frames. At
  `0x20` the game doesn't notice the traffic, but frames can't be grabbed fast
  enough. Not solved yet; the plan is to take the frame in one DMA at high
  priority and send it at low priority.

## Sound

- Undertale outputs 4 channels with `sceAudioOutputPannedBlocking`, 1024
  samples per block, 44.1 kHz stereo: 43.07 calls per second per channel
  (all of the music is actually on one of them).
- Hooks call the original first and copy the block afterwards. Waking the
  stream thread from the hook for every block was a bad idea: it outranks the
  game's audio thread and delayed it.
- With 16-bit conversion at `0x10`, the PSP's sound output itself ran ~2.6 %
  slow (41.96 calls/s instead of 43.07). The hooks saw every call; the
  hardware simply consumed slower, unnoticeable on the PSP speaker, but the PC
  drained its buffer every 1.5–2 s. The conversion keeps the CPU busy for
  ~2.4 ms stretches above the system's sound thread. Sleeping a little after
  every 16 converted rows fixes it:

  | pause | calls/s | game fps | frame send |
  |---|---|---|---|
  | 0 | 42.04 | 29.3 | 19.9 ms |
  | 50 µs | 43.07 | 29.9 | 19.1 ms |
  | 200 µs | 43.06 | 29.8 | 19.1 ms |
  | 1000 µs | 43.06 | 29.7 | 30.6 ms (tearing) |

- Undertale sometimes hands in a single all-zero block in the middle of the
  music, ~6 times a minute, regardless of the stream settings. The client
  fills such a block (the previous block played backwards, faded into the
  next one) before it's played.
- The PSP's and the PC's clocks differ by ~0.3 %. Dropping/repeating single
  samples works but can click; the client resamples instead, within ±0.5 %,
  steered slowly by the buffer level (16-tap windowed sinc; plain linear
  interpolation audibly dulled the sound).

## Debugging tools in the client

- `--trace [s]` prints every packet.
- `--dump DIR` records every received audio block and what the sound device
  played; Space in the window drops a marker. `tools/audio_dump.py DIR`
  looks for gaps, silent blocks and jumps at block joins.
- `--control FILE` re-reads FILE twice a second and applies the options in
  it without reconnecting — handy for A/B comparisons while playing.
