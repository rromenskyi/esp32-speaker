"""Keep only clips Whisper hears as the wake word; writes a TSV of all results.

    python asr_filter.py words/<word>.yaml <in_glob> <keep_dir> <results.tsv> [--negative]
--negative keeps clips that are NOT heard as the wake word (for confusables).
Resumable: clips already in the TSV are skipped.
"""
import glob, os, re, shutil, sys, time
import mlx_whisper
import yaml

word = yaml.safe_load(open(sys.argv[1]))
WAKE = re.compile(word["asr_match"])
LANG = word["asr_language"]
MODEL = "mlx-community/whisper-large-v3-turbo"

files = sorted(glob.glob(sys.argv[2]))
keep_dir, tsv = sys.argv[3], sys.argv[4]
negative = "--negative" in sys.argv
os.makedirs(keep_dir, exist_ok=True)
done = set()
if os.path.exists(tsv):
    done = {line.split("\t")[0] for line in open(tsv)}
kept, t0 = 0, time.time()
with open(tsv, "a") as out:
    for i, f in enumerate(files):
        if f in done:
            continue
        text = mlx_whisper.transcribe(f, path_or_hf_repo=MODEL, language=LANG, verbose=None,
                                      condition_on_previous_text=False)["text"].strip()
        hit = bool(WAKE.search(text.lower()))
        out.write(f"{f}\t{int(hit)}\t{text}\n")
        if hit != negative:
            shutil.copy(f, os.path.join(keep_dir, os.path.basename(os.path.dirname(f)) + "_" + os.path.basename(f)))
            kept += 1
        if i % 200 == 0:
            out.flush()
            print(f"{i}/{len(files)} kept {kept} ({time.time() - t0:.0f}s)", flush=True)
print(f"done: kept {kept} of {len(files)}")
