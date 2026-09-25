# Device ⇄ server protocol (v1)

The speaker is a thin voice front-end: it captures speech, plays audio and shows
state. Everything else — speech recognition, the LLM/agent, speech synthesis —
lives on the server. Any server that speaks this protocol can drive the speaker.

## Transport

* One **WebSocket** connection, opened by the device and kept alive.
  `ws://` on a trusted LAN, `wss://` otherwise.
* The URL is a device setting (`server <url> [token]` on the console).
* If a token is set, the device sends `Authorization: Bearer <token>` in the
  handshake. Servers should reject unauthenticated devices when exposed beyond
  a trusted LAN.
* The device reconnects on any failure after 3 s. Liveness uses WebSocket
  ping/pong frames (ping every 10 s, link dropped after 25 s without pong).
* **Text frames** carry JSON control messages, each with a `type` field.
  **Binary frames** carry audio.

## Audio

| | |
|---|---|
| Codec | `pcm16` — signed 16-bit little-endian PCM (v1). `opus` is reserved for v2 |
| Sample rate | 16000 Hz |
| Channels | 1 |
| Frame | 20 ms per binary message (640 bytes of `pcm16`) |

Binary frames from the device are microphone audio and are only sent between
`listen start` and `listen stop`. Binary frames from the server are playback
audio and are only valid between `speak start` and `speak stop`. Server audio
may arrive faster than real time; the device buffers it and applies TCP back
pressure when its buffer is full.

## Messages

### device → server

```jsonc
// First message after connecting.
{"type": "hello", "protocol": 1, "device": "esp32-speaker-b2add8", "firmware": "2c62432",
 "audio": {"codec": "pcm16", "rate": 16000, "channels": 1, "frame_ms": 20},
 "capabilities": {"buttons": ["k1", "k2", "k3", "boot"], "leds": 7}}

// Microphone streaming starts / stops. `reason` says why:
// "button" (push-to-talk), "wake" (wake word), "server" (server asked), "silence",
// "timeout" (utterance too long), "abort".
{"type": "listen", "state": "start", "reason": "button"}
{"type": "listen", "state": "stop", "reason": "button"}

// The device finished playing everything received for the current `speak`.
{"type": "speak", "state": "done"}

// The user interrupted (button, or wake word while speaking). The device has
// already stopped playback and flushed its buffer; the server should stop
// generating and discard pending audio.
{"type": "abort", "reason": "button"}

// Raw button events the device doesn't consume itself (k1 is push-to-talk and
// k2/k3 are volume -/+, handled locally; the others are forwarded).
{"type": "button", "name": "k2", "action": "press"}   // press | release
```

### server → device

```jsonc
// Reply to hello. `session` is opaque, for logs.
{"type": "hello", "protocol": 1, "session": "a1b2c3"}

// Playback of the following binary frames begins / has been fully sent.
{"type": "speak", "state": "start"}
{"type": "speak", "state": "stop"}

// Ask the device to open the microphone (e.g. a follow-up question without a
// wake word), or to stop listening (server-side end-of-utterance detection).
{"type": "listen", "state": "start"}
{"type": "listen", "state": "stop"}

// The server is working on a reply (the device shows a "thinking" pattern
// until speak start or an error). Servers should repeat it every few seconds
// (Bosun: 5 s) while working: the device gives up after 45 s without one, so
// a slow agent is fine as long as the server is alive.
{"type": "thinking"}

// Device settings.
{"type": "set", "volume": 70}

// Something went wrong; `message` is for logs. The device shows an error
// pattern briefly and returns to idle.
{"type": "error", "message": "stt failed"}
```

Unknown message types and fields must be ignored by both sides, so either can
be extended without breaking the other.

## A turn

```
device                                   server
  |-- listen start (button) ------------->|
  |== audio frames (20 ms) ==============>|
  |-- listen stop (button) -------------->|
  |<----------------------------- thinking|   (STT, agent, TTS)
  |<-------------------------- speak start|
  |<=============== audio frames ========|
  |<--------------------------- speak stop|
  |-- speak done ------------------------>|   (after the last sample played)
```

While speaking, the device keeps listening for an interruption (button press
now; wake word once echo cancellation lands). On interruption it stops
playback, flushes its buffer and sends `abort`.

## Device states and LEDs

| State | LEDs |
|-------|------|
| idle, connected | heartbeat: faint green double blink every 8 s |
| idle, auto (wake word) mode | faint slow cyan pulse |
| listening | solid blue |
| thinking | purple breathing |
| speaking | soft green |
| server unreachable | short red pulse every 5 s |
| Wi-Fi provisioning / joining | see [HARDWARE.md](HARDWARE.md#leds) |

## Reference server

`server/echo_server.py` implements the server side minimally: it records each
utterance and plays it straight back. It is a test tool for the device, not a
voice assistant.
