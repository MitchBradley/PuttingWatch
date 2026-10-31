"""WAV -> 16 kHz mono 16-bit PCM conditioning for make_voice.py (copied from
PuttingWatch/tools/wav2c_common.py).

Each source WAV (Piper output, any rate / mono or stereo / 16-bit PCM) is
resampled to RATE, silence-trimmed, normalised, and given short raised-cosine
fades. stdlib only.
"""
import math
import struct
import wave

RATE = 16000  # must match audio.rs SAMPLE_RATE


def read_wav_mono(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2, f"{path}: expected 16-bit PCM"
        ch, sr, n = w.getnchannels(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    s = struct.unpack("<%dh" % (len(raw) // 2), raw)
    if ch == 2:
        s = [(s[i] + s[i + 1]) // 2 for i in range(0, len(s), 2)]
    return list(s), sr


def resample(s, src_rate, dst_rate):
    if src_rate == dst_rate:
        return s
    ratio = src_rate / dst_rate
    if ratio > 1.0:  # downsampling - cheap box pre-filter to tame aliasing
        k = int(math.ceil(ratio))
        s = [sum(s[max(0, i - k + 1):i + 1]) / min(k, i + 1) for i in range(len(s))]
    out = []
    n_out = int(len(s) / ratio)
    for i in range(n_out):
        x = i * ratio
        i0 = int(x)
        i1 = min(i0 + 1, len(s) - 1)
        f = x - i0
        out.append(s[i0] * (1 - f) + s[i1] * f)
    return out


def trim(s, pad_ms=6):
    peak = max((abs(v) for v in s), default=1)
    thr = max(150, peak * 0.02)
    lo, hi = 0, len(s)
    while lo < len(s) and abs(s[lo]) < thr:
        lo += 1
    while hi > lo and abs(s[hi - 1]) < thr:
        hi -= 1
    pad = int(RATE * pad_ms / 1000)
    return s[max(0, lo - pad):min(len(s), hi + pad)]


def normalize(s, target=0.80):  # headroom - the codec + amp path clips near full-scale
    peak = max((abs(v) for v in s), default=1)
    g = (target * 32767.0) / peak if peak else 1.0
    return [max(-32768, min(32767, int(round(v * g)))) for v in s]


def fade(s, ms=5):
    n = min(int(RATE * ms / 1000), len(s) // 2)
    for i in range(n):
        e = 0.5 - 0.5 * math.cos(math.pi * i / n)
        s[i] = int(s[i] * e)
        s[-1 - i] = int(s[-1 - i] * e)
    return s


def load_sample(path):
    """Full conditioning chain: returns a list of int16 at RATE."""
    s, sr = read_wav_mono(path)
    return fade(normalize(trim(resample(s, sr, RATE))))
