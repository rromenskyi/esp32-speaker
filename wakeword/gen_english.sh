#!/bin/bash
# Multi-speaker samples from the English LibriTTS Piper model (904 voices),
# driven by the phoneme/text variants in the word file.
#   ./gen_english.sh words/<word>.yaml <out_dir> <samples_per_variant>
set -euo pipefail
word=$1 out=$2 n=$3
. .venv/bin/activate
python - "$word" <<'PY' | while IFS=$'\t' read -r kind value; do
import sys, yaml
w = yaml.safe_load(open(sys.argv[1]))
for p in w.get("phoneme_variants", []): print(f"ph\t{p}")
for t in w.get("text_variants", []): print(f"tx\t{t}")
PY
  i=$((${i:-0} + 1))
  flags=(); [ "$kind" = ph ] && flags=(--phoneme-input)
  python piper-sample-generator/generate_samples.py "$value" "${flags[@]}" --max-samples "$n" --batch-size 64 \
    --noise-scales 0.667 0.9 --noise-scale-ws 0.8 1.0 --length-scales 0.85 1.0 1.15 \
    --slerp-weights 0.3 0.5 0.7 --output-dir "$out/v$i"
done
