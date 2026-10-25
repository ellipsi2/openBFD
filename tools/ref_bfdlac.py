#!/usr/bin/env python3
"""Reference decoder for .bfdlac (BFDLAC v2/v3) — used to pin down semantics
before porting to C++. Not a product component.

Usage:
  ref_bfdlac.py decode <file.bfdlac> [--wav out.wav] [--stats]
  ref_bfdlac.py loudness <file.bfdlac>...
"""
import json, math, struct, sys
from pathlib import Path

TAB = json.load(open(Path(__file__).resolve().parents[2] / "analysis/bfdlac_tables.json"))
V2 = TAB["v2"]
V1 = TAB["v1"]
BITS = [4, 8, 12, 16, 2, 6, 10, 14, 1, 3, 5, 7, 9, 11, 13, 15]
BLOCK_SAMPLES = [16, 32, 64, 128]


class BitReader:
    """LSB-first fields inside big-endian... no: fields consume bit 0 of each
    byte first (matches BitStreamEncoder::DecodeBitsAtBitDepth)."""
    __slots__ = ("data", "pos", "bits_left")

    def __init__(self, data, pos=0):
        self.data = data
        self.pos = pos
        self.bits_left = 8

    def read_signed(self, bits):
        if bits == 0:
            return 0
        # 32-bit little-endian window at byte pos
        end = self.pos + 4
        w = int.from_bytes(self.data[self.pos:end].ljust(4, b"\0"), "little")
        bit_pos = 8 - self.bits_left
        val = (w << (32 - bits - bit_pos)) & 0xFFFFFFFF
        # arithmetic: sign-extend from bit 31 of the masked value
        val >>= 32 - bits
        if val >= (1 << (bits - 1)):
            val -= (1 << bits)
        # advance
        used = bit_pos + bits
        self.pos += used >> 3
        self.bits_left = 8 - (used & 7) if (used & 7) else 8
        return val

    def align_byte(self):
        if 1 <= self.bits_left <= 7:
            self.read_signed(self.bits_left)

    def read_uint_le(self, nbytes):
        v = int.from_bytes(self.data[self.pos:self.pos + nbytes].ljust(nbytes, b"\0"), "little")
        self.pos += nbytes
        return v

    def read_int_le(self, nbytes):
        v = self.read_uint_le(nbytes)
        bits = nbytes * 8
        if v >= (1 << (bits - 1)):
            v -= (1 << bits)
        return v


def decode_block(payload, scheme, bit_depth, version, block_samples):
    """Decode one channel sub-block. payload starts after the scheme byte.
    Returns (list[block_samples] ints, bits_consumed_from_payload)."""
    alg = (scheme >> 4) & 0xF if version >= 2 else (scheme >> 2) & 0xF
    err = scheme & 0xF if version >= 2 else scheme & 3
    bits = BITS[err]
    bps = 3 if bit_depth >= 20 else 2
    r = BitReader(payload)
    out = [0] * block_samples

    if alg == 0:  # raw
        for i in range(block_samples):
            out[i] = r.read_int_le(bps)
    elif alg == 1:  # constant
        v = r.read_int_le(bps)
        for i in range(block_samples):
            out[i] = v
    elif alg == 2:  # median (= seed + per-sample residual)
        s = r.read_int_le(bps)
        for i in range(block_samples):
            out[i] = s + r.read_signed(bits)
    elif alg == 3:  # gradient
        s0 = r.read_int_le(bps)
        s1 = r.read_int_le(bps)
        for i in range(block_samples):
            out[i] = s1 + s0 * i + r.read_signed(bits)
    elif alg == 4:  # zeros
        pass
    elif alg == 5:  # diff2 (2nd order delta)
        a = r.read_int_le(bps)
        b = r.read_int_le(bps)
        out[0], out[1] = a, b
        for i in range(2, block_samples):
            out[i] = 2 * out[i - 1] - out[i - 2] + r.read_signed(bits)
    elif alg == 6:  # value quads (groups of 4)
        for g in range(block_samples // 4):
            s = r.read_int_le(bps)
            out[g * 4] = s
            for k in range(1, 4):
                out[g * 4 + k] = s + r.read_signed(bits)
    elif alg == 7:  # diff (1st order delta)
        s = r.read_int_le(bps)
        out[0] = s
        acc = s
        for i in range(1, block_samples):
            acc += r.read_signed(bits)
            out[i] = acc
    elif alg == 8:  # value octs (groups of 8)
        for g in range(block_samples // 8):
            s = r.read_int_le(bps)
            out[g * 8] = s
            for k in range(1, 8):
                out[g * 8 + k] = s + r.read_signed(bits)
    elif alg == 9:  # hdiff2: half-slope extrapolation (2 seeds + n-2 residuals)
        a = r.read_int_le(bps)
        b = r.read_int_le(bps)
        out[0], out[1] = a, b
        for i in range(2, block_samples):
            out[i] = out[i - 1] + ((out[i - 1] - out[i - 2]) >> 1) + r.read_signed(bits)
    elif alg == 10:  # quadratic
        s0 = r.read_int_le(bps)
        s1 = r.read_int_le(bps)
        s2 = r.read_int_le(bps)
        for i in range(block_samples):
            out[i] = s2 + (s1 + s0 * i) * i + r.read_signed(bits)
    elif alg == 11:  # shorten p3
        a = r.read_int_le(bps)
        b = r.read_int_le(bps)
        c = r.read_int_le(bps)
        out[0], out[1], out[2] = a, b, c
        for i in range(3, block_samples):
            out[i] = out[i - 3] + 3 * (out[i - 1] - out[i - 2]) + r.read_signed(bits)
    elif alg == 12:  # diff, 16-bit seed
        s = r.read_int_le(2)
        out[0] = s
        acc = s
        for i in range(1, block_samples):
            acc += r.read_signed(bits)
            out[i] = acc
    elif alg == 13:  # diff2, 16-bit seeds
        a = r.read_int_le(2)
        b = r.read_int_le(2)
        out[0], out[1] = a, b
        for i in range(2, block_samples):
            out[i] = 2 * out[i - 1] - out[i - 2] + r.read_signed(bits)
    elif alg == 14:  # diff2, 8-bit seeds
        a = r.read_int_le(1)
        b = r.read_int_le(1)
        out[0], out[1] = a, b
        for i in range(2, block_samples):
            out[i] = 2 * out[i - 1] - out[i - 2] + r.read_signed(bits)
    elif alg == 15:  # diff, 8-bit seed
        s = r.read_int_le(1)
        out[0] = s
        acc = s
        for i in range(1, block_samples):
            acc += r.read_signed(bits)
            out[i] = acc
    else:
        raise ValueError(alg)
    r.align_byte()
    return out


def parse_container(data):
    if data[:4] != b"BFDC":
        raise ValueError("not BFDC")
    pos, chunks = 8, {}
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        chunks[cid] = (pos + 8, size)
        pos += 8 + size
    fmt, _ = chunks[b"fmt "]
    bit_depth, channels, frames, sample_rate, extra = struct.unpack(">5I", data[fmt:fmt + 20])
    body, size = chunks[b"Indx"]
    vals = [struct.unpack(">I", data[body + 4 * i:body + 4 * i + 4])[0] for i in range(size // 4)]
    block_size, frame_count = vals[0], vals[1]
    offsets = vals[2:2 + frame_count]
    dstart, dsize = chunks[b"data"]
    return dict(bit_depth=bit_depth, channels=channels, frames=frames,
                sample_rate=sample_rate, extra=extra, block_size=block_size,
                frame_count=frame_count, offsets=offsets, dstart=dstart, dsize=dsize)


def decode_file(path, lsb_first=True):
    data = Path(path).read_bytes()
    hdr = parse_container(data)
    ch = hdr["channels"]
    frames = hdr["frames"]
    block = hdr["block_size"]
    bps = 3 if hdr["bit_depth"] >= 20 else 2
    group = bps - 1
    table = (V2 if True else V1)[group * 256:(group + 1) * 256]
    out = []
    alg_use = {}
    for fi in range(hdr["frame_count"]):
        start = hdr["dstart"] + hdr["offsets"][fi]
        end = hdr["dstart"] + (hdr["offsets"][fi + 1] if fi + 1 < hdr["frame_count"] else hdr["dsize"])
        frame = data[start:end]
        remaining = frames - fi * block
        subs = min(16, (remaining + 63) // 64)
        pos = 0
        sub_frames = []
        for _ in range(subs):
            n = frame[pos]
            assert n == ch, (n, ch)
            pos += 1
            channels = []
            for c in range(n):
                scheme = frame[pos]
                size = table[scheme]
                payload = frame[pos + 1:pos + size]
                alg_use[scheme >> 4] = alg_use.get(scheme >> 4, 0) + 1
                vals = decode_block(payload, scheme, hdr["bit_depth"], 3, 64)
                channels.append(vals)
                pos += size
            sub_frames.append(channels)
        assert pos == len(frame), (fi, pos, len(frame))
        for sf in sub_frames:
            out.append(sf)
    # interleave: out[sample][channel], scale 24-bit values -> float
    scale = 1.0 / (1 << (hdr["bit_depth"] - 1))
    n = len(out) * 64
    samples = [[0.0] * ch for _ in range(len(out) * 64)]
    for si, sf in enumerate(out):
        for c in range(ch):
            for k in range(64):
                samples[si * 64 + k][c] = sf[c][k] * scale
    return hdr, samples[:frames], alg_use


def rms_db(samples, c):
    acc = 0.0
    n = 0
    for s in samples:
        v = s[c]
        acc += v * v
        n += 1
    if acc <= 0:
        return -999.0
    return 10 * math.log10(acc / n)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    cmd = args[0]
    if cmd == "decode":
        path = args[1]
        hdr, samples, alg_use = decode_file(path)
        print(f"{path}: {hdr['frames']} frames, {hdr['channels']} ch, {hdr['sample_rate']} Hz")
        print("algorithms:", dict(sorted(alg_use.items())))
        for c in range(min(hdr["channels"], 12)):
            print(f"  ch{c:2d} rms {rms_db(samples, c):8.2f} dB   peak {max(abs(s[c]) for s in samples):.5f}")
        mono = [sum(s) / len(s) for s in samples]
        if "--wav" in args:
            import wave
            out = args[args.index("--wav") + 1]
            with wave.open(out, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(hdr["sample_rate"])
                w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(v * 32767)))) for v in mono))
            print("wrote", out)
    elif cmd == "loudness":
        for path in args[1:]:
            hdr, samples, _ = decode_file(path)
            # whole-file RMS over all channels
            acc = 0.0
            n = 0
            for s in samples:
                for v in s:
                    acc += v * v
                    n += 1
            db = 10 * math.log10(acc / n) if acc > 0 else -999
            print(f"{Path(path).name:24s} rms_all={db:8.2f} dB  frames={hdr['frames']}")


if __name__ == "__main__":
    main()
