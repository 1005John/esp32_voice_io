#pragma once

// Copy to secrets.h and fill in your values. secrets.h is git-ignored.

// Wi-Fi (open network: leave WIFI_PASSWORD empty)
#define WIFI_SSID "CMCC-IOT"
#define WIFI_PASSWORD ""

// Xiaomi MiMo Token Plan (CN) endpoint. Both ASR and TTS use /v1/chat/completions.
#define MIMO_HOST "token-plan-cn.xiaomimimo.com"
#define MIMO_PORT 443
#define MIMO_API_KEY ""                 // tp-... key; or set via serial: mimokey <key>
#define MIMO_ASR_MODEL "mimo-v2.5-asr"
#define MIMO_TTS_MODEL "mimo-v2.5-tts"
#define MIMO_TTS_VOICE "mimo_default"  // mimo_default|冰糖|茉莉|苏打|白桦|Mia|Chloe|Milo|Dean

// PC agent address (the LAN PC that types text into the focused input box and
// forwards agent text for TTS). Set via serial: pcip <ip> <port>
#define PC_AGENT_HOST ""                // e.g. 192.168.137.202
#define PC_AGENT_PORT 8765

// This board's own HTTP server, receiving /speak {text} from the PC agent.
#define DEVICE_HTTP_PORT 8766
