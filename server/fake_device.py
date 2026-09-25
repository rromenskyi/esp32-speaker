#!/usr/bin/env python3
"""Simulated speaker for docs/PROTOCOL.md: test a server without the hardware.

Connects like the firmware does, sends one utterance (a WAV file, or text spoken
by macOS `say`), and saves the server's spoken reply as a WAV.

    python fake_device.py ws://host:8080/api/device --say "What time is it?" --voice Samantha --out reply.wav
    python fake_device.py ws://host:8790/ --wav question.wav [--token SECRET]

Input audio is converted to 16 kHz mono pcm16 with ffmpeg and streamed in real
time as 20 ms frames, like the device's microphone.
"""
import argparse
import asyncio
import json
import os
import subprocess
import tempfile
import time
import wave

import websockets

RATE = 16000
FRAME_BYTES = RATE * 2 * 20 // 1000


def to_pcm16(path):
    return subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path,
         "-f", "s16le", "-acodec", "pcm_s16le", "-ar", str(RATE), "-ac", "1", "pipe:1"],
        check=True, capture_output=True).stdout


def say_to_pcm16(text, voice):
    with tempfile.TemporaryDirectory() as d:
        aiff = os.path.join(d, "q.aiff")
        cmd = ["say", "-o", aiff] + (["-v", voice] if voice else []) + [text]
        subprocess.run(cmd, check=True)
        return to_pcm16(aiff)


async def run(args):
    pcm = say_to_pcm16(args.say, args.voice) if args.say else to_pcm16(args.wav)
    headers = {"Authorization": f"Bearer {args.token}"} if args.token else {}
    t = {}
    async with websockets.connect(args.url, additional_headers=headers, max_size=None) as ws:
        await ws.send(json.dumps({"type": "hello", "protocol": 1, "device": args.device, "firmware": "fake",
                                  "audio": {"codec": "pcm16", "rate": RATE, "channels": 1, "frame_ms": 20}}))
        print("server:", await ws.recv())
        await ws.send(json.dumps({"type": "listen", "state": "start", "reason": "button"}))
        for i in range(0, len(pcm), FRAME_BYTES):
            await ws.send(pcm[i:i + FRAME_BYTES])
            await asyncio.sleep(0.02)                  # real time, like the mic
        await ws.send(json.dumps({"type": "listen", "state": "stop", "reason": "button"}))
        t["stop"] = time.monotonic()
        print(f"sent {len(pcm) / (RATE * 2):.2f} s of speech")
        reply = bytearray()
        while True:
            msg = await asyncio.wait_for(ws.recv(), timeout=args.timeout)
            if isinstance(msg, bytes):
                if "first_audio" not in t:
                    t["first_audio"] = time.monotonic()
                reply.extend(msg)
                continue
            m = json.loads(msg)
            print(f"+{time.monotonic() - t['stop']:5.2f}s server: {m}")
            if m.get("type") == "error":
                break
            if m.get("type") == "speak" and m.get("state") == "stop":
                await ws.send(json.dumps({"type": "speak", "state": "done"}))
                break
    if reply:
        with wave.open(args.out, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(bytes(reply))
        print(f"reply: {len(reply) / (RATE * 2):.2f} s saved to {args.out}; "
              f"first audio {t['first_audio'] - t['stop']:.2f} s after listen stop")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("url")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--say", help="text to speak with macOS `say`")
    src.add_argument("--wav", help="audio file to send (any format ffmpeg reads)")
    ap.add_argument("--voice", help="`say` voice, e.g. Samantha (en_US) or any installed voice (`say -v '?'`)")
    ap.add_argument("--device", default="fake-speaker")
    ap.add_argument("--token")
    ap.add_argument("--out", default="reply.wav")
    ap.add_argument("--timeout", type=float, default=120)
    asyncio.run(run(ap.parse_args()))


if __name__ == "__main__":
    main()
