"""Positives in running speech: another speaker's speech before the wake word
(--prefix: "hey, <word>", "..., <word>"), and/or after it (the request that
follows straight away: "<word>, turn on the music").

    python make_continuations.py <positives_dir> <speech_dir> <out_dir> <N> [--prefix] [--suffix]
(no flag = --suffix, the original behavior)
"""
import glob, os, random, sys, wave
import numpy as np
import soundfile as sf

def read(path):
    # FLEURS ships float32 wav; generated clips are pcm16. Normalize to int16 scale.
    x, _ = sf.read(path, dtype="float32", always_2d=False)
    return x * 32767.0

def trim(x, thr=0.02):
    e = np.abs(x) > thr * np.abs(x).max()
    idx = np.nonzero(e)[0]
    return x[idx[0]:idx[-1] + 1] if len(idx) else x

pos_dir, speech_dir, out, n = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
prefix = "--prefix" in sys.argv
suffix = "--suffix" in sys.argv or not prefix
os.makedirs(out, exist_ok=True)
pos = sorted(glob.glob(os.path.join(pos_dir, "*.wav")))
speech = sorted(glob.glob(os.path.join(speech_dir, "*.wav")))
rnd = random.Random(7)
for i in range(n):
    p = trim(read(rnd.choice(pos)))

    def snippet(min_s, max_s):
        s = trim(read(rnd.choice(speech)))
        start = rnd.randint(0, max(0, len(s) - int(16000 * max_s)))
        s = s[start:start + rnd.randint(int(16000 * min_s), int(16000 * max_s))]
        return s * (np.abs(p).max() / max(np.abs(s).max(), 1)) * rnd.uniform(0.6, 1.1)

    gap = lambda: np.zeros(int(16000 * rnd.uniform(0.05, 0.3)), dtype=np.float32)
    parts = [p]
    if prefix:
        parts = [snippet(0.3, 1.5), gap()] + parts
    if suffix:
        parts = parts + [gap(), snippet(0.25, 1.0)]
    x = np.clip(np.concatenate(parts), -32768, 32767).astype(np.int16)
    with wave.open(os.path.join(out, f"ctx_{i:05d}.wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(x.tobytes())
print("done", n)
