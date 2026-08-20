# esp32_voice_io

Turn an ESP32-S3 + INMP441 + I2S amplifier into a **voice I/O peripheral** for
any LAN PC. Speak → text is typed into the PC's focused input box. The PC's
local agent text → spoken through the board's speaker. Cloud ASR/TTS is
[Xiaomi MiMo](https://mimo.mi.com) (`mimo-v2.5-asr` / `mimo-v2.5-tts`), OpenAI
chat-completions compatible.

```
┌─────────────┐  WAV base64        ┌──────────────┐  SSE text   ┌────────────┐
│  ESP32-S3   │ ──────────────────▶│  MiMo cloud  │ ──────────▶ │  ESP32-S3  │
│ INMP441 mic │◀─ SSE pcm16 (TTS) ─│ token-plan-cn│             │  → PC /asr │
│ I2S amp 24k │                    │  /v1/chat/…  │             └─────┬──────┘
│ BOOT · WiFi │                    └──────────────┘                   │ HTTP
└──────┬──────┘                                                       │
       │ HTTP (LAN)                                                    │
┌──────▼──────────────────────────────────────────────────────────────────▼────┐
│  PC agent (Mac first, then any LAN PC)                                       │
│  /asr  → Unicode keyboard inject into current focus (the "光标" input box)    │
│  /speak → forward agent text to ESP32 for MiMo TTS streaming playback        │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Hardware (Seeed XIAO ESP32-S3 + INMP441 audio module)

| Device | Signal | GPIO |
| --- | --- | --- |
| INMP441 mic | SCK/BCLK | 4 |
| INMP441 mic | WS/LRCLK | 5 |
| INMP441 mic | SD | 6 |
| I2S amp | DIN | 7 |
| I2S amp | BCLK | 15 |
| I2S amp | LRCLK | 16 |
| On-board RGB | data | 48 |
| BOOT button | — | 0 |

- Mic records 16 kHz / 32-bit / mono → uploaded as WAV (base64) to MiMo ASR.
- Speaker plays 24 kHz / 16-bit PCM16LE from MiMo TTS directly (no resample).

## Layout

- `esp32/` — Arduino firmware (`voice_io.ino`, `secrets.example.h`)
- `pc-agent/` — LAN PC agent (Python; `voice_io_agent.py`)
- `docs/protocol.md` — ESP32 ↔ PC HTTP protocol

## Bring-up (Mac verification)

1. `cp esp32/secrets.example.h esp32/secrets.h`, fill Wi-Fi + MiMo key.
2. Flash (see `esp32/README.md`), open serial console, run:
   `mimokey tp-…` · `pcip 192.168.137.202 8765` · `status`
3. Run the PC agent: `python3 pc-agent/voice_io_agent.py --esp 192.168.137.xxx`
4. Click a text box on the PC, hold BOOT on the board, speak, release. Text is
   typed into the box. POST `{"text":"…"}` to `http://127.0.0.1:8765/speak` to
   hear it spoken.

See `docs/protocol.md` for the message format.
