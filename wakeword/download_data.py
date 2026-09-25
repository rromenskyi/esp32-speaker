"""Augmentation + negative data for wake word training (see README.md for
licenses: the mix is for personal, non-commercial models)."""
import os, subprocess, tarfile, zipfile, urllib.request
import numpy as np, scipy.io.wavfile
from tqdm import tqdm

def fetch(url, path):
    if not os.path.exists(path):
        print("downloading", url, flush=True)
        urllib.request.urlretrieve(url, path)

from huggingface_hub import snapshot_download, hf_hub_download

# Room impulse responses (MIT), already 16 kHz wav files in the repo
if not os.path.isdir("mit_rirs") or not os.listdir("mit_rirs"):
    d = snapshot_download("davidscripka/MIT_environmental_impulse_responses", repo_type="dataset", allow_patterns=["16khz/*"])
    os.makedirs("mit_rirs", exist_ok=True)
    for f in os.listdir(os.path.join(d, "16khz")):
        os.symlink(os.path.join(d, "16khz", f), os.path.join("mit_rirs", f))

# Two AudioSet parquet shards for background noise, decoded to 16 kHz mono wav
if not os.path.isdir("audioset_16k") or not os.listdir("audioset_16k"):
    import pyarrow.parquet as pq
    os.makedirs("audioset_16k", exist_ok=True)
    for shard in ["00", "01"]:
        path = hf_hub_download("agkphysics/AudioSet", f"data/bal_train/{shard}.parquet", repo_type="dataset")
        table = pq.read_table(path)
        col = [c for c in table.column_names if "audio" in c.lower()][0]
        for i, cell in enumerate(tqdm(table.column(col).to_pylist())):
            data = cell["bytes"] if isinstance(cell, dict) else cell
            out = f"audioset_16k/{shard}_{i:05d}.wav"
            subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", "pipe:0", "-ar", "16000", "-ac", "1", out],
                           input=data, check=False)

# Precomputed negative spectrogram features (microWakeWord)
os.makedirs("negative_datasets", exist_ok=True)
for name in ["dinner_party.zip", "dinner_party_eval.zip", "no_speech.zip", "speech.zip"]:
    path = os.path.join("negative_datasets", name)
    if not os.path.isdir(path[:-4]):
        fetch("https://huggingface.co/datasets/kahrendt/microwakeword/resolve/main/" + name, path)
        zipfile.ZipFile(path).extractall("negative_datasets")

# Native-language read speech (FLEURS, CC-BY-4.0) as negatives: raw 16 kHz wav
# tarballs. FLEURS_LANG picks the language (default ru_ru).
lang = os.environ.get("FLEURS_LANG", "ru_ru")
if not os.path.isdir("fleurs_" + lang[:2]):
    os.makedirs("fleurs_" + lang[:2])
    for split in ["train", "dev"]:
        tgz = hf_hub_download("google/fleurs", f"data/{lang}/audio/{split}.tar.gz", repo_type="dataset")
        tarfile.open(tgz).extractall("fleurs_" + lang[:2])
print("data ready")
