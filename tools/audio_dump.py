"""Analyse a sound dump of `pspcap.exe --dump DIR` (stage 5 dropout hunting).

Files in DIR:
  in.bin         records of 40 bytes (see DumpRecord in pc/src/audio_out.cpp):
                 kind u32 (0 = block, 1 = user marker), source u32, samples u32,
                 channels u32, bytes u32, vol_left u16, vol_right u16,
                 psp_time_us u64, pc_time_us u64; blocks are followed by
                 `bytes` of s16 PCM as the PSP sent it
  out.raw        s16 stereo 44.1 kHz, what the sound device played
  out_times.bin  pairs of u64: device frame index, PC time at that callback

Usage: python audio_dump.py DIR [--wav]
Prints, per source, gaps in the PSP timestamps and delivery delays, waveform
jumps at block joins, repeated or silent blocks, and for each user marker
what happened in the 1.5 s before it. --wav also writes DIR/out.wav.
"""

import struct
import sys
import wave
from array import array
from pathlib import Path

RATE = 44100
REC = struct.Struct("<5I2H2Q")


def read_in(path):
    blocks, marks = [], []
    data = path.read_bytes()
    pos = 0
    while pos + REC.size <= len(data):
        kind, src, samples, ch, nbytes, vl, vr, psp_us, pc_us = REC.unpack_from(data, pos)
        pos += REC.size
        if kind == 1:
            marks.append((src, pc_us))
            continue
        if pos + nbytes > len(data):
            break  # cut off while the client was still writing
        pcm = array("h")
        pcm.frombytes(data[pos:pos + nbytes])
        pos += nbytes
        blocks.append(dict(src=src, samples=samples, ch=ch, vl=vl, vr=vr,
                           psp=psp_us, pc=pc_us, pcm=pcm))
    return blocks, marks


def frame(pcm, ch, i):
    return (pcm[i * ch], pcm[i * ch + ch - 1])


def analyse_source(src, blocks):
    print(f"\nsource {src}: {len(blocks)} blocks")
    psp_gaps, pc_delays, joins, events = [], [], [], []
    inner = []
    for b in blocks[:200]:
        ch, pcm = b["ch"], b["pcm"]
        for i in range(1, b["samples"]):
            inner.append(abs(frame(pcm, ch, i)[0] - frame(pcm, ch, i - 1)[0]))
    inner.sort()
    typical = inner[len(inner) * 99 // 100] if inner else 0
    first_pc = blocks[0]["pc"] - blocks[0]["psp"]
    for prev, b in zip(blocks, blocks[1:]):
        gap = (b["psp"] - prev["psp"]) / 1000
        psp_gaps.append(gap)
        delay = (b["pc"] - b["psp"]) - first_pc
        pc_delays.append(delay / 1000)
        ch = b["ch"]
        jump = abs(frame(b["pcm"], ch, 0)[0] - frame(prev["pcm"], prev["ch"], prev["samples"] - 1)[0])
        joins.append(jump)
        if gap > 30:
            events.append((b["pc"], f"PSP gap {gap:.1f} ms before this block"))
        if b["pcm"] == prev["pcm"] and any(b["pcm"]):
            events.append((b["pc"], "block repeats the previous one exactly"))
        if typical and jump > 4 * typical and jump > 2000:
            events.append((b["pc"], f"waveform jump {jump} at the join (99% inside blocks <= {typical})"))
    psp_gaps.sort()
    print(f"  PSP time between blocks: median {psp_gaps[len(psp_gaps)//2]:.2f} ms, "
          f"max {psp_gaps[-1]:.2f} ms, >30 ms: {sum(g > 30 for g in psp_gaps)}")
    d = sorted(pc_delays)
    print(f"  delivery delay vs first block: min {d[0]:.1f} ms, median {d[len(d)//2]:.1f}, "
          f"max {d[-1]:.1f} ms")
    silent = sum(1 for b in blocks if not any(b["pcm"]))
    print(f"  silent blocks: {silent}; join jumps > 4x typical: "
          f"{sum(1 for j in joins if typical and j > 4 * typical and j > 2000)}")
    return events


def analyse_out(dirpath):
    raw = (dirpath / "out.raw").read_bytes()
    out = array("h")
    out.frombytes(raw[: len(raw) // 4 * 4])
    times = []
    tdata = (dirpath / "out_times.bin").read_bytes()
    for i in range(0, len(tdata) // 16 * 16, 16):
        times.append(struct.unpack_from("<2Q", tdata, i))
    return out, times


def out_events(out, times):
    """Stretches of exact digital silence after sound (what an underrun sounds like)."""
    events = []
    n = len(out) // 2
    run = 0
    for i in range(n):
        if out[2 * i] == 0 and out[2 * i + 1] == 0:
            run += 1
            continue
        if run >= 64 and i - run > 0:
            events.append((i - run, run))
        run = 0
    return events


def frame_to_pc(times, idx):
    lo, hi = 0, len(times) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if times[mid][0] <= idx:
            lo = mid
        else:
            hi = mid - 1
    f, t = times[lo]
    return t + (idx - f) * 1_000_000 // RATE


def main():
    dirpath = Path(sys.argv[1])
    blocks, marks = read_in(dirpath / "in.bin")
    by_src = {}
    for b in blocks:
        by_src.setdefault(b["src"], []).append(b)
    events = []
    for src in sorted(by_src):
        events += [(t, f"src {src}: {msg}") for t, msg in analyse_source(src, by_src[src])]
    out, times = analyse_out(dirpath)
    if times:
        for idx, run in out_events(out, times):
            events.append((frame_to_pc(times, idx), f"output: {run / RATE * 1000:.1f} ms of silence"))
    events.sort()
    t0 = blocks[0]["pc"] if blocks else 0
    print(f"\n{len(events)} events, {len(marks)} user markers")
    for t, msg in events[:60]:
        print(f"  {(t - t0) / 1e6:8.3f} s  {msg}")
    for n, t in marks:
        near = [(et, m) for et, m in events if t - 1_500_000 <= et <= t + 200_000]
        print(f"\nMARK {n} at {(t - t0) / 1e6:.3f} s: {len(near)} events in the 1.5 s before")
        for et, m in near:
            print(f"    {(et - t) / 1000:+8.1f} ms  {m}")
    if "--wav" in sys.argv:
        with wave.open(str(dirpath / "out.wav"), "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(out.tobytes())
        print(f"\nwrote {dirpath / 'out.wav'}")


if __name__ == "__main__":
    main()
