"""Positives followed by speech: wake word, a short gap, then another speaker's
speech, cut shortly after the word. Teaches the model that the word can be
followed straight away by the request ("<word>, turn on the music").

    python make_continuations.py <positives_dir> <speech_dir> <out_dir> <N>
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
os.makedirs(out, exist_ok=True)
pos = sorted(glob.glob(os.path.join(pos_dir, "*.wav")))
speech = sorted(glob.glob(os.path.join(speech_dir, "*.wav")))
rnd = random.Random(7)
for i in range(n):
    p = trim(read(rnd.choice(pos)))
    s = read(rnd.choice(speech))
    s = trim(s)
    start = rnd.randint(0, max(0, len(s) - 16000))
    s = s[start:start + rnd.randint(4000, 16000)]          # 0.25-1 s of speech
    s *= (np.abs(p).max() / max(np.abs(s).max(), 1)) * rnd.uniform(0.6, 1.1)
    gap = np.zeros(int(16000 * rnd.uniform(0.05, 0.3)), dtype=np.float32)
    x = np.clip(np.concatenate([p, gap, s]), -32768, 32767).astype(np.int16)
    with wave.open(os.path.join(out, f"cont_{i:05d}.wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(x.tobytes())
print("done", n)
