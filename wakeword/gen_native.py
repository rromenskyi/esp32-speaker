"""Native-language samples: Piper voices + macOS `say`, 16 kHz mono.

    python gen_native.py words/<word>.yaml pos out_dir N   # wake word phrases
    python gen_native.py words/<word>.yaml neg out_dir N   # confusable words / phrases

Voices are read from voices/<name>.onnx (download from rhasspy/piper-voices).
"""
import glob, os, random, subprocess, sys, tempfile, wave
import yaml
import numpy as np
from piper import PiperVoice, SynthesisConfig
from scipy.signal import resample_poly


def to16k(x, sr):
    g = np.gcd(sr, 16000)
    return resample_poly(x.astype(np.float32), 16000 // g, sr // g)

def save(path, x):
    x = np.clip(x, -32768, 32767).astype(np.int16)
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(x.tobytes())

def main():
    word = yaml.safe_load(open(sys.argv[1]))
    kind, out, n = sys.argv[2], sys.argv[3], int(sys.argv[4])
    os.makedirs(out, exist_ok=True)
    phrases = word["positive_phrases"] if kind == "pos" else word["negative_phrases"]
    voices = [PiperVoice.load(f"voices/{v}.onnx") for v in word["native_voices"]]
    rnd = random.Random(1 if kind == "pos" else 2)
    for i in range(n):
        text = rnd.choice(phrases)
        if i % 6 == 5 and word.get("say_voice"):   # every sixth sample from macOS `say`
            with tempfile.TemporaryDirectory() as d:
                aiff = os.path.join(d, "s.aiff")
                subprocess.run(["say", "-v", word["say_voice"], "-r", str(rnd.randint(140, 260)), "-o", aiff, text], check=True)
                raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", aiff, "-f", "s16le", "-ar", "16000", "-ac", "1", "pipe:1"],
                                     check=True, capture_output=True).stdout
                x = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
        else:
            v = rnd.choice(voices)
            cfg = SynthesisConfig(length_scale=rnd.uniform(0.8, 1.3), noise_scale=rnd.uniform(0.4, 0.9),
                                  noise_w_scale=rnd.uniform(0.5, 1.0))
            chunks = list(v.synthesize(text, syn_config=cfg))
            x = np.concatenate([c.audio_int16_array for c in chunks]).astype(np.float32)
            x = to16k(x, chunks[0].sample_rate)
        save(os.path.join(out, f"{kind}_{i:05d}.wav"), x * rnd.uniform(0.5, 1.0))
    print("done", n, out)

if __name__ == "__main__":
    main()
