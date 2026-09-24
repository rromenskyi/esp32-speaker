#!/usr/bin/env python3
"""Reference server for docs/PROTOCOL.md: plays every utterance straight back.

A test tool for the device, not an assistant. It exercises the whole path
(mic -> Wi-Fi -> server -> Wi-Fi -> speaker), saves each utterance as a WAV file
and logs timings.

    pip install -r requirements.txt
    python echo_server.py --port 8765 [--token SECRET] [--save recordings/]

Then on the device console:  server ws://<this-host>:8765/ [SECRET]
"""
import argparse
import array
import asyncio
import json
import os
import time
import uuid
import wave

import websockets

RATE = 16000
FRAME_BYTES = RATE * 2 * 20 // 1000   # 20 ms of pcm16 mono


def normalize(pcm, peak_dbfs=-3.0, max_gain=20.0):
    """Scale pcm16 so its peak sits at peak_dbfs (like a TTS engine's output level)."""
    samples = array.array("h", pcm)
    peak = max((abs(v) for v in samples), default=0)
    if not peak:
        return pcm
    gain = min(32767 * 10 ** (peak_dbfs / 20) / peak, max_gain)
    return array.array("h", (int(v * gain) for v in samples)).tobytes()


def log(dev, msg):
    print(f"{time.strftime('%H:%M:%S')} [{dev}] {msg}", flush=True)


async def handle(ws, args):
    if args.token:
        auth = ws.request.headers.get("Authorization", "")
        if auth != f"Bearer {args.token}":
            await ws.close(code=4001, reason="bad token")
            return
    dev = "?"
    utterance = None
    t_stop = None
    async for msg in ws:
        if isinstance(msg, bytes):
            if utterance is not None:
                utterance.extend(msg)
            continue
        m = json.loads(msg)
        t = m.get("type")
        if t == "hello":
            dev = m.get("device", "?")
            log(dev, f"hello firmware={m.get('firmware')} audio={m.get('audio')}")
            await ws.send(json.dumps({"type": "hello", "protocol": 1, "session": uuid.uuid4().hex[:8]}))
        elif t == "listen" and m.get("state") == "start":
            utterance = bytearray()
            log(dev, f"listening ({m.get('reason')})")
        elif t == "listen" and m.get("state") == "stop" and utterance is not None:
            t_stop = time.monotonic()
            audio, utterance = bytes(utterance), None
            secs = len(audio) / (RATE * 2)
            log(dev, f"utterance {secs:.2f} s ({m.get('reason')})")
            if args.save:
                os.makedirs(args.save, exist_ok=True)
                path = os.path.join(args.save, time.strftime("%Y%m%d-%H%M%S") + ".wav")
                with wave.open(path, "wb") as w:
                    w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(audio)
                log(dev, f"saved {path}")
            audio = normalize(audio)
            await ws.send(json.dumps({"type": "thinking"}))
            await ws.send(json.dumps({"type": "speak", "state": "start"}))
            # Send faster than real time (like a TTS engine would); the device
            # buffers and pushes back over TCP when full.
            for i in range(0, len(audio), FRAME_BYTES):
                await ws.send(audio[i:i + FRAME_BYTES])
                if i % (FRAME_BYTES * 25) == 0:
                    await asyncio.sleep(0)
            await ws.send(json.dumps({"type": "speak", "state": "stop"}))
            log(dev, f"reply sent in {time.monotonic() - t_stop:.2f} s")
        elif t == "speak" and m.get("state") == "done":
            if t_stop:
                log(dev, f"playback done {time.monotonic() - t_stop:.2f} s after listen stop")
        elif t == "abort":
            log(dev, f"abort ({m.get('reason')})")
        else:
            log(dev, f"{m}")
    log(dev, "disconnected")


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--token", help="require 'Authorization: Bearer <token>'")
    ap.add_argument("--save", metavar="DIR", help="save each utterance as a WAV file")
    args = ap.parse_args()
    async with websockets.serve(lambda ws: handle(ws, args), args.host, args.port, max_size=None):
        print(f"echo server on ws://{args.host}:{args.port}/", flush=True)
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
