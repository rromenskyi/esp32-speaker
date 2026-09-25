"""Build microWakeWord feature sets (RaggedMmap spectrograms).

    python make_features.py <clips_dir> <out_dir> [--augment] [--reps N]

Writes out_dir/{training,validation,testing}/wakeword_mmap. Augmentation mixes
in AudioSet noise, MIT room impulse responses, EQ/pitch/gain, as in the
microWakeWord notebook.
"""
import argparse, os
from mmap_ninja.ragged import RaggedMmap
from microwakeword.audio.augmentation import Augmentation
from microwakeword.audio.clips import Clips
from microwakeword.audio.spectrograms import SpectrogramGeneration

ap = argparse.ArgumentParser()
ap.add_argument("clips"); ap.add_argument("out")
ap.add_argument("--augment", action="store_true")
ap.add_argument("--reps", type=int, default=2)
ap.add_argument("--duration", type=float, default=3.2)
a = ap.parse_args()

clips = Clips(input_directory=a.clips, file_pattern="*.wav", max_clip_duration_s=None,
              remove_silence=False, random_split_seed=10, split_count=0.1)
aug = Augmentation(
    augmentation_duration_s=a.duration,
    augmentation_probabilities={
        "SevenBandParametricEQ": 0.1, "TanhDistortion": 0.1, "PitchShift": 0.15, "BandStopFilter": 0.1,
        "AddColorNoise": 0.1, "AddBackgroundNoise": 0.75 if a.augment else 0.0, "Gain": 1.0,
        "RIR": 0.5 if a.augment else 0.0,
    },
    impulse_paths=["mit_rirs"], background_paths=["audioset_16k"],
    background_min_snr_db=-5, background_max_snr_db=10, min_jitter_s=0.195, max_jitter_s=0.205)

for split, name, reps, slide in [("training", "train", a.reps, 10), ("validation", "validation", 1, 10),
                                 ("testing", "test", 1, 1)]:
    out = os.path.join(a.out, split, "wakeword_mmap")
    if os.path.exists(out):
        continue
    os.makedirs(os.path.dirname(out), exist_ok=True)
    gen = SpectrogramGeneration(clips=clips, augmenter=aug, slide_frames=slide, step_ms=10)
    RaggedMmap.from_generator(out_dir=out, sample_generator=gen.spectrogram_generator(split=name, repeat=reps),
                              batch_size=100, verbose=True)
    print("wrote", out, flush=True)
