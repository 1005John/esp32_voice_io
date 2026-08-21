/*
 * esp32_voice_io — ESP32-S3 voice I/O peripheral for a LAN PC.
 *
 * Hold BOOT: INMP441 records 16 kHz (LED blue). Release: WAV is base64-POSTed
 * to MiMo mimo-v2.5-asr (stream); each SSE text delta is POSTed to the PC agent
 * /asr, which types it into the PC's focused input box.
 * The PC agent POSTs /speak {text}; the text goes to MiMo mimo-v2.5-tts
 * (stream, pcm16) and the 24 kHz PCM plays through the I2S amp as it arrives
 * (LED purple).
 *
 * Mic: BCLK=4, WS=5, SD=6. Speaker: DIN=7, BCLK=15, LRCLK=16. RGB=48.
 * Serial (115200) UART0 GPIO44/43:
 *   mimokey <key>   pcip <ip> <port>   ttsvoice <name>   playtext <text>
 *   status   help
 */
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <driver/i2s.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include "secrets.h"

#ifndef MIMO_API_KEY
#define MIMO_API_KEY ""
#endif
#ifndef MIMO_HOST
#define MIMO_HOST "token-plan-cn.xiaomimimo.com"
#endif
#ifndef MIMO_PORT
#define MIMO_PORT 443
#endif
#ifndef MIMO_ASR_MODEL
#define MIMO_ASR_MODEL "mimo-v2.5-asr"
#endif
#ifndef MIMO_TTS_MODEL
#define MIMO_TTS_MODEL "mimo-v2.5-tts"
#endif
#ifndef MIMO_TTS_VOICE
#define MIMO_TTS_VOICE "mimo_default"
#endif
#ifndef PC_AGENT_HOST
#define PC_AGENT_HOST ""
#endif
#ifndef PC_AGENT_PORT
#define PC_AGENT_PORT 8765
#endif
#ifndef DEVICE_HTTP_PORT
#define DEVICE_HTTP_PORT 8766
#endif

// Serial defaults to USB CDC (CDCOnBoot=cdc). Use the right USB-C port for
// the console; the left "COM" CH340 port (UART0 GPIO44/43) is left free.

namespace {

constexpr uint8_t BOOT_PIN = 0;
constexpr uint8_t RGB_LED_PIN = 48;
constexpr int MIC_BCLK_PIN = 4, MIC_WS_PIN = 5, MIC_DATA_PIN = 6;
constexpr i2s_port_t MIC_PORT = I2S_NUM_0;
constexpr i2s_port_t SPEAKER_PORT = I2S_NUM_1;
constexpr int SPEAKER_DATA_PIN = 7, SPEAKER_BCLK_PIN = 15, SPEAKER_WS_PIN = 16;
constexpr uint32_t MIC_SAMPLE_RATE = 16000;
constexpr uint32_t SPEAKER_SAMPLE_RATE = 24000;
constexpr uint16_t MIC_BITS = 32;
constexpr uint8_t CHANNELS = 1;
constexpr uint32_t MAX_RECORD_SECONDS = 12;
constexpr size_t MAX_RECORD_BYTES = MIC_SAMPLE_RATE * (MIC_BITS / 8) * MAX_RECORD_SECONDS;
constexpr uint32_t MIN_RECORD_BYTES = MIC_SAMPLE_RATE * (MIC_BITS / 8) / 3;
constexpr uint32_t DEBOUNCE_MS = 35, WIFI_RETRY_MS = 10000;
constexpr size_t SPEAKER_FRAMES_PER_WRITE = 512;
constexpr uint8_t SPEAKER_VOLUME_PERCENT = 25;
// Cap for downloaded TTS PCM (kept in PSRAM, played after Wi-Fi is paused).
constexpr size_t MAX_REPLY_AUDIO_BYTES = 2 * 1024 * 1024;
constexpr size_t MAX_B64_LINE = 65536;

Preferences preferences;
uint8_t* recordingBuffer = nullptr;
size_t recordedBytes = 0;
bool recording = false, stablePressed = false, lastRawPressed = false;
volatile bool voiceJobActive = false;
bool speakerInstalled = false, asrUploading = false, playingTts = false;
uint32_t rawChangedAt = 0, lastWifiAttemptMs = 0;
String mimoKey, pcHost, ttsVoice = MIMO_TTS_VOICE, pendingSpeakText;
uint16_t pcPort = PC_AGENT_PORT;

int16_t speakerMono[SPEAKER_FRAMES_PER_WRITE];
int16_t speakerStereo[SPEAKER_FRAMES_PER_WRITE * 2];
uint8_t ledR = 255, ledG = 255, ledB = 255;
WebServer server(DEVICE_HTTP_PORT);

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  if (r == ledR && g == ledG && b == ledB) return;
  neopixelWrite(RGB_LED_PIN, r, g, b);
  ledR = r; ledG = g; ledB = b;
}
void updateLed() {
  if (recording) { setLed(0, 0, 255); return; }
  if (playingTts) { setLed(180, 0, 255); return; }
  if (asrUploading) { setLed(255, 120, 0); return; }
  if (WiFi.status() == WL_CONNECTED) { setLed(0, 255, 0); return; }
  setLed(0, 0, 0);
}
void configureBootButton() { pinMode(BOOT_PIN, INPUT_PULLUP); }

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (strlen(WIFI_PASSWORD) == 0) WiFi.begin(WIFI_SSID);
  else WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptMs = millis();
  Serial.printf("[vio] connecting to Wi-Fi %s\n", WIFI_SSID);
}
void updateWifi() {
  if (WiFi.status() == WL_CONNECTED) { updateLed(); return; }
  if (millis() - lastWifiAttemptMs >= WIFI_RETRY_MS) { WiFi.disconnect(); connectWifi(); }
  updateLed();
}

void installMicrophone() {
  const i2s_config_t config = {
    .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8, .dma_buf_len = 256,
    .use_apll = false, .tx_desc_auto_clear = false, .fixed_mclk = 0,
  };
  const i2s_pin_config_t pins = {
    .bck_io_num = MIC_BCLK_PIN, .ws_io_num = MIC_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = MIC_DATA_PIN,
  };
  ESP_ERROR_CHECK(i2s_driver_install(MIC_PORT, &config, 0, nullptr));
  ESP_ERROR_CHECK(i2s_set_pin(MIC_PORT, &pins));
  i2s_zero_dma_buffer(MIC_PORT);
}
void installSpeaker() {
  const i2s_config_t config = {
    .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SPEAKER_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8, .dma_buf_len = 256,
    .use_apll = false, .tx_desc_auto_clear = true, .fixed_mclk = 0,
  };
  const i2s_pin_config_t pins = {
    .bck_io_num = SPEAKER_BCLK_PIN, .ws_io_num = SPEAKER_WS_PIN,
    .data_out_num = SPEAKER_DATA_PIN, .data_in_num = I2S_PIN_NO_CHANGE,
  };
  ESP_ERROR_CHECK(i2s_driver_install(SPEAKER_PORT, &config, 0, nullptr));
  ESP_ERROR_CHECK(i2s_set_pin(SPEAKER_PORT, &pins));
  i2s_zero_dma_buffer(SPEAKER_PORT);
}
void ensureSpeaker() {
  if (speakerInstalled) return;
  installSpeaker();
  speakerInstalled = true;
  configureBootButton();
  Serial.println("[vio] I2S speaker ready");
}
int16_t speakerSample(int16_t s) {
  return static_cast<int16_t>((static_cast<int32_t>(s) * SPEAKER_VOLUME_PERCENT) / 100);
}
bool writeSpeakerFrames(size_t frames) {
  const size_t bytes = frames * sizeof(int16_t) * 2;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(speakerStereo);
  size_t sent = 0;
  while (sent < bytes) {
    size_t w = 0;
    const esp_err_t r = i2s_write(SPEAKER_PORT, p + sent, bytes - sent, &w, pdMS_TO_TICKS(1000));
    if (r != ESP_OK || w == 0) {
      Serial.printf("[vio] I2S write fail r=%d w=%u/%u\n", (int)r, (unsigned)w, (unsigned)(bytes - sent));
      return false;
    }
    sent += w;
  }
  return true;
}

void writeLe16(uint8_t* t, uint16_t v) { t[0] = v & 0xff; t[1] = (v >> 8) & 0xff; }
void writeLe32(uint8_t* t, uint32_t v) { t[0] = v & 0xff; t[1] = (v >> 8) & 0xff; t[2] = (v >> 16) & 0xff; t[3] = (v >> 24) & 0xff; }
void makeWavHeader(uint8_t (&h)[44], size_t pcmBytes) {
  memcpy(h, "RIFF", 4);
  writeLe32(h + 4, 36 + pcmBytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  writeLe32(h + 16, 16);
  writeLe16(h + 20, 1);
  writeLe16(h + 22, CHANNELS);
  writeLe32(h + 24, MIC_SAMPLE_RATE);
  writeLe32(h + 28, MIC_SAMPLE_RATE * CHANNELS * MIC_BITS / 8);
  writeLe16(h + 32, CHANNELS * MIC_BITS / 8);
  writeLe16(h + 34, MIC_BITS);
  memcpy(h + 36, "data", 4);
  writeLe32(h + 40, pcmBytes);
}

bool writeAll(WiFiClient& c, const uint8_t* d, size_t l) {
  while (l > 0) { const size_t w = c.write(d, l); if (w == 0) return false; d += w; l -= w; }
  return true;
}
bool writeAll(WiFiClient& c, const String& s) { return writeAll(c, reinterpret_cast<const uint8_t*>(s.c_str()), s.length()); }
bool writeAll(WiFiClientSecure& c, const uint8_t* d, size_t l) {
  while (l > 0) { const size_t w = c.write(d, l); if (w == 0) return false; d += w; l -= w; }
  return true;
}
bool writeAll(WiFiClientSecure& c, const String& s) { return writeAll(c, reinterpret_cast<const uint8_t*>(s.c_str()), s.length()); }

String jsonStringField(const String& j, const char* field) {
  const String key = String("\"") + field + "\"";
  int s = j.indexOf(key);
  if (s < 0) return "";
  s = j.indexOf(':', s + key.length());
  if (s < 0) return "";
  int p = s + 1;
  while (p < (int)j.length() && (j[p] == ' ' || j[p] == '\t')) ++p;
  if (p >= (int)j.length() || j[p] != '"') return "";  // null / number / bool
  const int e = j.indexOf('"', p + 1);
  return e < 0 ? "" : j.substring(p + 1, e);
}
String jsonEscape(const String& s) {
  String out; out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

bool readResponseHeaders(WiFiClient& c, uint32_t timeoutMs = 15000) {
  const uint32_t deadline = millis() + timeoutMs;
  String h;
  while (static_cast<int32_t>(deadline - millis()) > 0) {
    while (c.available() && static_cast<int32_t>(deadline - millis()) > 0) {
      const char ch = static_cast<char>(c.read());
      h += ch;
      if (h.endsWith("\r\n\r\n")) {
        const bool ok = h.startsWith("HTTP/1.1 2") || h.startsWith("HTTP/1.0 2");
        if (!ok) Serial.printf("[vio] HTTP resp: %.180s\n", h.c_str());
        return ok;
      }
      if (h.length() > 4096) return false;
    }
    if (!c.connected()) break;
    delay(2);
  }
  return false;
}

void forwardAsrToPc(const String& text, bool sendEnter) {
  if (WiFi.status() != WL_CONNECTED || pcHost.length() == 0) return;
  if (text.length() == 0 && !sendEnter) return;
  WiFiClient c;
  c.setTimeout(2000);
  if (!c.connect(pcHost.c_str(), pcPort)) { Serial.println("[vio] cannot reach PC /asr"); return; }
  const String body = "{\"text\":\"" + jsonEscape(text) + "\",\"enter\":" + (sendEnter ? "true" : "false") + "}";
  const String req = "POST /asr HTTP/1.1\r\nHost: " + pcHost + ":" + String(pcPort) +
      "\r\nContent-Type: application/json\r\nContent-Length: " + String(body.length()) +
      "\r\nConnection: close\r\n\r\n";
  if (writeAll(c, req)) writeAll(c, body);
  // Fire-and-forget: the body is already in the TCP send buffer, so we don't
  // wait for the PC's HTTP response. This saves ~1.5s per delta and keeps
  // streaming text low-latency. Closing sends FIN; the agent reads to EOF.
  delay(20);
  c.stop();
}

void uploadAsrAndForward() {
  if (recordedBytes < MIN_RECORD_BYTES) { Serial.println("[vio] recording too short"); return; }
  if (mimoKey.length() == 0) { Serial.println("[vio] MiMo key missing; use: mimokey <key>"); return; }
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[vio] Wi-Fi not connected"); return; }

  uint8_t wavHeader[44];
  makeWavHeader(wavHeader, recordedBytes);
  const size_t wavBytes = sizeof(wavHeader) + recordedBytes;
  uint8_t* wav = static_cast<uint8_t*>(ps_malloc(wavBytes));
  if (!wav) { Serial.println("[vio] wav alloc failed"); return; }
  memcpy(wav, wavHeader, sizeof(wavHeader));
  memcpy(wav + sizeof(wavHeader), recordingBuffer, recordedBytes);

  const size_t b64Cap = (wavBytes + 2) / 3 * 4 + 1;
  uint8_t* b64 = static_cast<uint8_t*>(ps_malloc(b64Cap));
  if (!b64) { free(wav); Serial.println("[vio] base64 alloc failed"); return; }
  size_t b64Len = 0;
  if (mbedtls_base64_encode(b64, b64Cap, &b64Len, wav, wavBytes) != 0) {
    free(wav); free(b64); Serial.println("[vio] base64 encode failed"); return;
  }
  free(wav);

  WiFiClientSecure tls;
  tls.setInsecure();
  tls.setTimeout(20);
  if (!tls.connect(MIMO_HOST, MIMO_PORT)) { Serial.println("[vio] cannot connect MiMo"); free(b64); return; }
  const String prefix = String("{\"model\":\"") + MIMO_ASR_MODEL +
      "\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\","
      "\"input_audio\":{\"data\":\"data:audio/wav;base64,";
  const String suffix = "\"}}]}],\"asr_options\":{\"language\":\"auto\"},\"stream\":true}";
  const size_t contentLength = prefix.length() + b64Len + suffix.length();
  const String head = String("POST /v1/chat/completions HTTP/1.1\r\nHost: ") + MIMO_HOST +
      "\r\nAuthorization: Bearer " + mimoKey +
      "\r\nContent-Type: application/json\r\nContent-Length: " + String(contentLength) +
      "\r\nConnection: close\r\nAccept: text/event-stream\r\n\r\n";
  if (!writeAll(tls, head) || !writeAll(tls, prefix) || !writeAll(tls, b64, b64Len) || !writeAll(tls, suffix)) {
    Serial.println("[vio] ASR write failed"); tls.stop(); free(b64); return;
  }
  free(b64);
  Serial.printf("[vio] ASR uploaded %.1f KiB; reading SSE\n", wavBytes / 1024.0f);

  if (!readResponseHeaders(tls)) { Serial.println("[vio] ASR HTTP error"); tls.stop(); return; }
  String line;
  line.reserve(MAX_B64_LINE);
  size_t forwarded = 0;
  const uint32_t deadline = millis() + 60000;
  while ((tls.connected() || tls.available()) && static_cast<int32_t>(deadline - millis()) > 0) {
    while (tls.available()) {
      const char ch = static_cast<char>(tls.read());
      if (ch == '\n') {
        if (line.startsWith("data:")) {
          String payload = line.substring(5); payload.trim();
          if (payload != "[DONE]") {
            const String delta = jsonStringField(payload, "content");
            if (delta.length()) { forwardAsrToPc(delta, false); ++forwarded; }
          }
        }
        line = "";
      } else if (ch != '\r') {
        line += ch;
        if (line.length() > MAX_B64_LINE) line = "";
      }
    }
    delay(1);
  }
  tls.stop();
  Serial.printf("[vio] ASR done; forwarded %u deltas\n", (unsigned)forwarded);
  if (forwarded > 0) { forwardAsrToPc("", true); Serial.println("[vio] ASR sentence end -> PC Enter"); }
}

// Decode every TTS audio chunk to PSRAM; Wi-Fi is paused during playback to
// avoid the USB brownout that happens when Wi-Fi RX and the I2S amp run together.
void playTts(const String& text) {
  if (text.length() == 0) return;
  if (mimoKey.length() == 0) { Serial.println("[vio] MiMo key missing"); return; }
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[vio] Wi-Fi not connected"); return; }

  WiFiClientSecure tls;
  tls.setInsecure();
  tls.setTimeout(20);
  if (!tls.connect(MIMO_HOST, MIMO_PORT)) { Serial.println("[vio] cannot connect MiMo TTS"); return; }
  const String body = String("{\"model\":\"") + MIMO_TTS_MODEL +
      "\",\"messages\":[{\"role\":\"assistant\",\"content\":\"" + jsonEscape(text) +
      "\"}],\"audio\":{\"format\":\"pcm16\",\"voice\":\"" + ttsVoice + "\"},\"stream\":true}";
  const String head = String("POST /v1/chat/completions HTTP/1.1\r\nHost: ") + MIMO_HOST +
      "\r\nAuthorization: Bearer " + mimoKey +
      "\r\nContent-Type: application/json\r\nContent-Length: " + String(body.length()) +
      "\r\nConnection: close\r\nAccept: text/event-stream\r\n\r\n";
  if (!writeAll(tls, head) || !writeAll(tls, body)) { Serial.println("[vio] TTS write failed"); tls.stop(); return; }
  if (!readResponseHeaders(tls)) { Serial.println("[vio] TTS HTTP error"); tls.stop(); return; }
  Serial.printf("[vio] TTS streaming %u chars\n", (unsigned)text.length());

  uint8_t* reply = static_cast<uint8_t*>(ps_malloc(MAX_REPLY_AUDIO_BYTES));
  if (!reply) { Serial.println("[vio] tts reply alloc failed"); tls.stop(); return; }
  size_t replyLen = 0;
  size_t chunks = 0;
  String line;
  line.reserve(MAX_B64_LINE);
  const uint32_t deadline = millis() + 90000;
  bool done = false;
  while ((tls.connected() || tls.available()) && static_cast<int32_t>(deadline - millis()) > 0 && !done) {
    while (tls.available()) {
      const char ch = static_cast<char>(tls.read());
      if (ch == '\n') {
        if (line.startsWith("data:")) {
          String payload = line.substring(5); payload.trim();
          if (payload == "[DONE]") { done = true; }
          else {
            const String audio = jsonStringField(payload, "data");
            if (audio.length()) {
              const size_t pcmCap = audio.length() * 3 / 4 + 4;
              uint8_t* pcm = static_cast<uint8_t*>(ps_malloc(pcmCap));
              if (pcm) {
                size_t pcmLen = 0;
                if (mbedtls_base64_decode(pcm, pcmCap, &pcmLen,
                    reinterpret_cast<const unsigned char*>(audio.c_str()), audio.length()) == 0 && pcmLen > 0) {
                  if (replyLen + pcmLen <= MAX_REPLY_AUDIO_BYTES) {
                    memcpy(reply + replyLen, pcm, pcmLen);
                    replyLen += pcmLen;
                    ++chunks;
                  }
                }
                free(pcm);
              }
            }
          }
        }
        line = "";
      } else if (ch != '\r') {
        line += ch;
        if (line.length() > MAX_B64_LINE) line = "";
      }
    }
    delay(1);
  }
  tls.stop();
  Serial.printf("[vio] TTS downloaded %u chunks / %u bytes; pausing Wi-Fi\n",
                (unsigned)chunks, (unsigned)replyLen);
  if (replyLen == 0) { free(reply); Serial.println("[vio] no TTS audio"); return; }

  WiFi.mode(WIFI_OFF);
  updateLed();
  delay(250);
  ensureSpeaker();
  playingTts = true;
  updateLed();
  const int16_t* src = reinterpret_cast<const int16_t*>(reply);
  const size_t frames = replyLen / 2;
  size_t off = 0;
  while (off < frames) {
    const size_t n = min(static_cast<size_t>(SPEAKER_FRAMES_PER_WRITE), frames - off);
    for (size_t i = 0; i < n; ++i) {
      const int16_t s = speakerSample(src[off + i]);
      speakerStereo[i * 2] = s;
      speakerStereo[i * 2 + 1] = s;
    }
    if (!writeSpeakerFrames(n)) break;
    off += n;
  }
  delay(180);
  i2s_zero_dma_buffer(SPEAKER_PORT);
  playingTts = false;
  const size_t played = off * 2;
  free(reply);
  WiFi.mode(WIFI_STA);
  connectWifi();
  updateLed();
  Serial.printf("[vio] TTS done; played %u/%u bytes\n", (unsigned)played, (unsigned)replyLen);
}

void asrTask(void*) { uploadAsrAndForward(); voiceJobActive = false; asrUploading = false; updateLed(); vTaskDelete(nullptr); }
void speakTask(void*) { String t = pendingSpeakText; pendingSpeakText = ""; playTts(t); voiceJobActive = false; vTaskDelete(nullptr); }

void queueAsrUpload() {
  if (voiceJobActive) { Serial.println("[vio] voice job in progress; ignored"); return; }
  voiceJobActive = true;
  if (xTaskCreate(asrTask, "vio-asr", 16384, nullptr, 1, nullptr) != pdPASS) { voiceJobActive = false; Serial.println("[vio] cannot start asr task"); }
}
void queueSpeak(const String& text) {
  if (voiceJobActive) { Serial.println("[vio] busy; speak ignored"); return; }
  voiceJobActive = true;
  pendingSpeakText = text;
  if (xTaskCreate(speakTask, "vio-tts", 16384, nullptr, 1, nullptr) != pdPASS) { voiceJobActive = false; pendingSpeakText = ""; Serial.println("[vio] cannot start tts task"); }
}

void beginRecording() {
  if (recording || recordingBuffer == nullptr) return;
  if (voiceJobActive) { Serial.println("[vio] voice job in progress; record ignored"); return; }
  recordedBytes = 0;
  recording = true;
  updateLed();
  i2s_zero_dma_buffer(MIC_PORT);
  Serial.println("[vio] recording started");
}
void captureAudio() {
  if (!recording || recordedBytes >= MAX_RECORD_BYTES) return;
  size_t read = 0;
  const size_t space = MAX_RECORD_BYTES - recordedBytes;
  const esp_err_t r = i2s_read(MIC_PORT, recordingBuffer + recordedBytes, min(space, static_cast<size_t>(4096)), &read, pdMS_TO_TICKS(20));
  if (r == ESP_OK && read > 0) recordedBytes += read;
  if (recordedBytes >= MAX_RECORD_BYTES) { recording = false; updateLed(); Serial.println("[vio] max recording length reached"); queueAsrUpload(); }
}
void finishRecording() {
  if (!recording) return;
  recording = false;
  asrUploading = true;
  updateLed();
  Serial.printf("[vio] recording stopped: %u bytes\n", (unsigned)recordedBytes);
  queueAsrUpload();
}
void handleButton() {
  const uint32_t now = millis();
  const bool down = digitalRead(BOOT_PIN) == LOW;
  if (down != lastRawPressed) { lastRawPressed = down; rawChangedAt = now; }
  if (now - rawChangedAt < DEBOUNCE_MS) return;
  if (stablePressed == down) return;
  stablePressed = down;
  updateLed();
  if (stablePressed && !recording) beginRecording();
  else if (!stablePressed && recording) finishRecording();
}

void handleSpeak() {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"no body\"}"); return; }
  const String body = server.arg("plain");
  const String text = jsonStringField(body, "text");
  if (text.length() == 0) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"no text\"}"); return; }
  if (voiceJobActive) { server.send(409, "application/json", "{\"ok\":false,\"busy\":true}"); return; }
  queueSpeak(text);
  server.send(200, "application/json", "{\"ok\":true,\"queued\":true}");
}
void handleHealth() { server.send(200, "application/json", "{\"ok\":true}"); }

void printStatus() {
  Serial.printf("[vio] wifi=%s ip=%s mimo=%s pc=%s:%u tts=%s rec=%s busy=%s boot=%s\n",
    WiFi.status() == WL_CONNECTED ? "up" : "offline",
    WiFi.localIP().toString().c_str(),
    mimoKey.length() ? "set" : "missing",
    pcHost.length() ? pcHost.c_str() : "missing", pcPort,
    ttsVoice.c_str(),
    recording ? "yes" : "no", voiceJobActive ? "yes" : "no",
    digitalRead(BOOT_PIN) == LOW ? "pressed" : "released");
}

void handleConsole() {
  if (!Serial.available()) return;
  String command = Serial.readStringUntil('\n');
  command.trim();
  if (command.startsWith("mimokey ") && command.length() > 8) {
    mimoKey = command.substring(8); mimoKey.trim();
    preferences.putString("mimo_key", mimoKey);
    Serial.println("[vio] MiMo key stored");
  } else if (command == "clear-mimokey") {
    mimoKey = ""; preferences.remove("mimo_key"); Serial.println("[vio] MiMo key cleared");
  } else if (command.startsWith("pcip ") && command.length() > 5) {
    String rest = command.substring(5); rest.trim();
    const int sp = rest.indexOf(' ');
    if (sp > 0) { pcHost = rest.substring(0, sp); pcPort = rest.substring(sp + 1).toInt(); }
    else { pcHost = rest; }
    if (pcPort == 0) pcPort = PC_AGENT_PORT;
    preferences.putString("pc_host", pcHost);
    preferences.putUShort("pc_port", pcPort);
    Serial.printf("[vio] PC agent = %s:%u\n", pcHost.c_str(), pcPort);
  } else if (command.startsWith("ttsvoice ") && command.length() > 9) {
    ttsVoice = command.substring(9); ttsVoice.trim();
    preferences.putString("tts_voice", ttsVoice);
    Serial.printf("[vio] TTS voice = %s\n", ttsVoice.c_str());
  } else if (command.startsWith("playtext ") && command.length() > 9) {
    const String text = command.substring(9);
    if (text.length()) { Serial.printf("[vio] playtext: %s\n", text.c_str()); queueSpeak(text); }
  } else if (command == "nettest") {
    Serial.printf("[vio] resolving %s ...\n", MIMO_HOST);
    IPAddress ip;
    if (WiFi.hostByName(MIMO_HOST, ip)) Serial.printf("[vio] DNS ok: %s\n", ip.toString().c_str());
    else Serial.println("[vio] DNS failed");
    { WiFiClientSecure tls; tls.setInsecure(); tls.setTimeout(15);
      if (tls.connect(MIMO_HOST, MIMO_PORT)) { Serial.println("[vio] TLS connect ok"); tls.stop(); }
      else Serial.println("[vio] TLS connect failed"); }
  } else if (command == "status") {
    printStatus();
  } else if (command == "help") {
    Serial.println("[vio] commands: mimokey <key>, clear-mimokey, pcip <ip> [port], ttsvoice <name>, playtext <text>, nettest, status, help");
  } else if (command.length()) {
    Serial.println("[vio] unknown command; type help");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);  // let USB CDC enumerate before the first log line
  configureBootButton();
  lastRawPressed = digitalRead(BOOT_PIN) == LOW;
  stablePressed = lastRawPressed;
  rawChangedAt = millis() - DEBOUNCE_MS;
  updateLed();
  preferences.begin("vio", false);
  mimoKey = preferences.getString("mimo_key", MIMO_API_KEY);
  pcHost = preferences.getString("pc_host", PC_AGENT_HOST);
  pcPort = preferences.getUShort("pc_port", PC_AGENT_PORT);
  ttsVoice = preferences.getString("tts_voice", MIMO_TTS_VOICE);
  if (psramFound()) recordingBuffer = static_cast<uint8_t*>(ps_malloc(MAX_RECORD_BYTES));
  if (recordingBuffer == nullptr) Serial.println("[vio] PSRAM alloc failed; recording unavailable");
  installMicrophone();
  configureBootButton();
  lastRawPressed = digitalRead(BOOT_PIN) == LOW;
  stablePressed = lastRawPressed;
  rawChangedAt = millis() - DEBOUNCE_MS;
  connectWifi();
  server.on("/speak", HTTP_POST, handleSpeak);
  server.on("/health", HTTP_GET, handleHealth);
  server.onNotFound([]() { server.send(404, "application/json", "{\"ok\":false}"); });
  server.begin();
  Serial.printf("[vio] ready; max recording=%us, http port=%u, reset=%d\n", MAX_RECORD_SECONDS, DEVICE_HTTP_PORT, (int)esp_reset_reason());
  printStatus();
}

void loop() {
  updateWifi();
  server.handleClient();
  handleConsole();
  handleButton();
  captureAudio();
  delay(1);
}
