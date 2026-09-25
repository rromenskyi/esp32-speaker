# Wake word training

Trains a [microWakeWord](https://github.com/kahrendt/microWakeWord) streaming
model for the firmware's on-device detector (`main/wakeword.cc`). Everything a
wake word needs is in one file, `words/<word>.yaml`: phonetic variants for the
multi-speaker English TTS, phrases for native-language TTS voices, confusable
words used as hard negatives, and the ASR pattern that validates samples.

## Pipeline

```sh
./setup.sh                                    # envs + tools (macOS arm64, uv)
. .venv/bin/activate && python download_data.py && deactivate   # RIRs, noise, negatives
W=words/starpom.yaml

# 1. Positives: many voices, then keep only what Whisper hears as the word
./gen_english.sh $W pos_raw 4000
. .venv-asr/bin/activate
python gen_native.py $W pos native_pos 3000
python asr_filter.py $W 'pos_raw/*/*.wav' pos_keep pos_raw.tsv
python asr_filter.py $W 'native_pos/*.wav' pos_keep native_pos.tsv
# 2. Hard negatives: confusable words, native voices
python gen_native.py $W neg native_neg 2000
deactivate

# 3. Features (augmented with noise + room reverb) and training
. .venv/bin/activate
python make_features.py pos_keep feat_pos --augment --reps 2
python make_features.py native_neg feat_neg --augment --reps 1
python make_features.py fleurs_clips feat_fleurs_ru --reps 1     # native-language speech
python train.py feat_pos feat_neg 20000
```

The model lands in
`trained_models/wakeword/tflite_stream_state_internal_quant/stream_state_internal_quant.tflite`;
the training log's cutoff table (false rejections vs. false accepts per hour)
gives the `probability_cutoff`. Upload it to the speaker without reflashing:

```sh
curl --data-binary @stream_state_internal_quant.tflite \
  "http://<speaker>/wwmodel?cutoff=0.85&window=5&arena=45000&name=starpom"
```

and test it without the microphone with `POST /wwtest` (see `../models/README.md`).

## Why the ASR filter

The English multi-speaker model gives voice diversity (904 speakers) but often
mispronounces a foreign word; the native voices pronounce it right but are
few and sometimes slur a single isolated word. Whisper (large-v3-turbo, MLX)
transcribes every generated clip and only clips it hears as the wake word are
kept — about 30% of them.

## Data licenses

| Data | Use | License |
|------|-----|---------|
| Piper LibriTTS-R model, Piper voices | synthetic positives / negatives | MIT (Piper); voice datasets vary |
| MIT environmental impulse responses | reverb augmentation | see the dataset card |
| AudioSet (2 shards) | background noise | CC-BY 4.0, clips from YouTube |
| microWakeWord negative features (speech, dinner party, no speech) | negatives | mixed, see the dataset card |
| FLEURS (native-language read speech) | negatives | CC-BY 4.0 |

Several sources carry non-commercial or unclear terms, so treat models trained
with this mix as **personal, non-commercial** — as the microWakeWord authors
advise for their own notebook.
