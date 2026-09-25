#!/bin/bash
# Training environment (macOS arm64 tested; Python 3.11 via uv).
#   .venv      microWakeWord (TensorFlow) + piper-sample-generator (torch, MPS)
#   .venv-asr  mlx-whisper (ASR filter) + piper-tts (native voices)
set -euo pipefail
[ -d microWakeWord ] || git clone -q https://github.com/kahrendt/microWakeWord
[ -d piper-sample-generator ] || git clone -q -b mps-support https://github.com/kahrendt/piper-sample-generator
# torch >= 2.6 loads with weights_only=True, which the (trusted, official) Piper
# checkpoint doesn't support.
sed -i '' 's/torch.load(model_path)/torch.load(model_path, weights_only=False)/' piper-sample-generator/generate_samples.py
M=piper-sample-generator/models/en_US-libritts_r-medium.pt
[ -s $M ] || curl -sL -o $M https://github.com/rhasspy/piper-sample-generator/releases/download/v2.0.0/en_US-libritts_r-medium.pt

uv venv -q -p 3.11 .venv
. .venv/bin/activate
uv pip install -q 'git+https://github.com/puddly/pymicro-features@puddly/minimum-cpp-version' \
  'git+https://github.com/whatsnowplaying/audio-metadata@d4ebb238e6a401bb1a5aaaac60c9e2b3cb30929f'
uv pip install -q -e ./microWakeWord
# datasets >= 4 decodes audio through torchcodec; 3.6 still uses soundfile.
uv pip install -q torch torchaudio piper-phonemize-cross==1.2.1 "datasets==3.6.0" tensorboard pyyaml
deactivate

uv venv -q -p 3.11 .venv-asr
. .venv-asr/bin/activate
uv pip install -q mlx-whisper piper-tts scipy pyyaml
echo "ready"
