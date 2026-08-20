# ESP32 ↔ PC Agent Protocol

All messages are HTTP/1.1 over the LAN. The PC agent and the ESP32 each run a
small HTTP server. Text is UTF-8 JSON.

## Roles

| Component | Acts as HTTP client to | Acts as HTTP server on |
| --- | --- | --- |
| ESP32-S3 | MiMo cloud (HTTPS 443) + PC agent | `DEVICE_HTTP_PORT` (default 8766) |
| PC agent | ESP32 (`DEVICE_HTTP_PORT`) | `PC_AGENT_PORT` (default 8765) |

## ESP32 → PC: ASR text delta

The ESP32 streams the recognized text to the PC as MiMo emits SSE deltas.

```
POST /asr HTTP/1.1
Host: <pc_agent_host>:<pc_agent_port>
Content-Type: application/json
Content-Length: <n>

{"text":"增量文字"}
```

The PC agent types `text` into the OS's current keyboard focus (Unicode inject,
bypassing any IME) so it lands in whatever input box the user clicked.

The ESP32 may send many such POSTs, one per delta. The PC agent responds `200 OK`
with body `{"ok":true}`. Failures are non-fatal; the ESP32 keeps streaming.

## PC → ESP32: speak (TTS)

Any local agent on the PC asks the PC agent (or the ESP32 directly) to speak:

```
POST /speak HTTP/1.1
Host: <esp32_ip>:<DEVICE_HTTP_PORT>
Content-Type: application/json
Content-Length: <n>

{"text":"要朗读的文字"}
```

The ESP32 replies `200 {"ok":true,"queued":true}` immediately and performs the
TTS call + I2S playback in a background task. If a voice job is already running
it replies `409 {"ok":false,"busy":true}`.

## Configuration stored on the ESP32 (NVS)

- `mimo_key`  — MiMo API key (tp-…)
- `pc_host`   — PC agent IP
- `pc_port`    — PC agent port

Set over serial (`115200`, UART0 GPIO44/43):

```
mimokey tp-xxxxxxxx
pcip 192.168.137.202 8765
status
help
```
