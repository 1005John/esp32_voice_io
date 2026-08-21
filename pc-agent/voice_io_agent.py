#!/usr/bin/env python3
"""esp32_voice_io PC agent.

POST /asr {text}  -> type `text` into the OS's current keyboard focus
                    (Unicode inject, bypassing any IME) so it lands in the
                    input box the user clicked (the "光标").
POST /speak {text} -> forward to the ESP32's /speak for MiMo TTS streaming.

Mac is implemented now (CGEvent Unicode string events). Windows/Linux are
stubbed (SendKeys/ydotool) and will be filled in for cross-PC support.

Run:
    python3 voice_io_agent.py --listen 8765 --esp 192.168.137.123
On macOS grant Accessibility to the terminal running this (System Settings >
Privacy & Security > Accessibility), or typing silently fails.
"""
import argparse
import ctypes
import ctypes.util
import json
import shutil
import subprocess
import sys
import urllib.request
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# --------------------------------------------------------------------------- #
# Keyboard Unicode inject
# --------------------------------------------------------------------------- #
_CG = None  # lazily-loaded CoreGraphics handle


def _mac_coregraphics():
    path = ctypes.util.find_library("CoreGraphics") or \
        "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics"
    return ctypes.CDLL(path)


def _mac_init():
    global _CG
    if _CG is not None:
        return _CG
    cg = _mac_coregraphics()
    _set = lambda n, r, a: setattr(getattr(cg, n), "__doc__", None) or \
        (setattr(getattr(cg, n), "restype", r), setattr(getattr(cg, n), "argtypes", a))
    specs = [
        ("CGEventSourceCreate", ctypes.c_void_p, [ctypes.c_uint32]),
        ("CGEventCreateKeyboardEvent", ctypes.c_void_p,
         [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_bool]),
        ("CGEventKeyboardSetUnicodeString", None,
         [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint16)]),
        ("CGEventPost", None, [ctypes.c_uint32, ctypes.c_void_p]),
    ]
    for name, rest, args in specs:
        fn = getattr(cg, name, None)
        if fn is None:
            raise RuntimeError(f"CoreGraphics missing {name}")
        fn.restype = rest
        fn.argtypes = args
    _CG = cg
    return cg


def mac_type(text):
    cg = _mac_init()
    # kCGEventSourceStateHIDSystemState = 1; kCGHIDEventTap = 0
    try:
        AppSvc = ctypes.CDLL("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices")
        print(f"[agent] AXIsProcessTrusted={bool(AppSvc.AXIsProcessTrusted())}", file=sys.stderr)
    except Exception as e:
        print(f"[agent] trust check error: {e}", file=sys.stderr)
    src = cg.CGEventSourceCreate(1)
    if not src:
        print("[agent] no CGEventSource (grant Accessibility permission)",
              file=sys.stderr)
        return False
    # CGEventKeyboardSetUnicodeString accepts ~20 UniChar per event.
    MAXCH = 20
    for i in range(0, len(text), MAXCH):
        chunk = text[i:i + MAXCH]
        n = len(chunk)
        buf = (ctypes.c_uint16 * n)(*[ord(c) for c in chunk])
        ev = cg.CGEventCreateKeyboardEvent(src, 0, True)  # keydown, keycode 0
        if ev:
            cg.CGEventKeyboardSetUnicodeString(ev, n, buf)
            cg.CGEventPost(0, ev)
            evup = cg.CGEventCreateKeyboardEvent(src, 0, False)  # keyup
            if evup:
                cg.CGEventKeyboardSetUnicodeString(evup, n, buf)
                cg.CGEventPost(0, evup)
    return True


def mac_press_key(keycode):
    cg = _mac_init()
    src = cg.CGEventSourceCreate(1)
    if not src:
        return False
    ev = cg.CGEventCreateKeyboardEvent(src, keycode, True)
    if ev:
        cg.CGEventPost(0, ev)
    evup = cg.CGEventCreateKeyboardEvent(src, keycode, False)
    if evup:
        cg.CGEventPost(0, evup)
    return True


def win_type(text):
    # Provisional; replace with SendInput(KEYEVENTF_UNICODE) for reliable CJK.
    import comtypes  # may not be installed
    raise RuntimeError("win_type not yet implemented")


def linux_type(text):
    tool = shutil.which("xdotool") or shutil.which("ydotool")
    if not tool:
        print("[agent] no xdotool/ydotool on PATH", file=sys.stderr)
        return False
    if tool.endswith("ydotool"):
        subprocess.run([tool, "type", "--", text], check=False)
    else:
        subprocess.run([tool, "type", "--clearmodifiers", "--", text], check=False)
    return True


def type_text(text):
    if not text:
        return True
    try:
        if sys.platform == "darwin":
            return mac_type(text)
        if sys.platform == "win32":
            return win_type(text)
        if sys.platform.startswith("linux"):
            return linux_type(text)
    except Exception as e:  # noqa
        print(f"[agent] type failed: {e}", file=sys.stderr)
        return False
    print(f"[agent] typing not supported on {sys.platform}", file=sys.stderr)
    return False


# --------------------------------------------------------------------------- #
# ESP32 forward
# --------------------------------------------------------------------------- #
ESP_HOST = None
ESP_PORT = 8766


def forward_speak(text):
    data = json.dumps({"text": text}).encode("utf-8")
    req = urllib.request.Request(
        f"http://{ESP_HOST}:{ESP_PORT}/speak",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=6) as r:
            return 200 <= r.status < 300
    except Exception as e:  # noqa
        print(f"[agent] forward /speak -> {ESP_HOST}:{ESP_PORT} failed: {e}",
              file=sys.stderr)
        return False


# --------------------------------------------------------------------------- #
# HTTP server
# --------------------------------------------------------------------------- #
class Handler(BaseHTTPRequestHandler):
    def _send_json(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ("/health", "/"):
            self._send_json(200, {"ok": True, "platform": sys.platform})
        else:
            self._send_json(404, {"ok": False})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            obj = json.loads(raw.decode("utf-8")) if raw else {}
        except Exception:
            obj = {}
        text = obj.get("text", "") or ""
        if self.path == "/asr":
            enter = bool(obj.get("enter", False))
            ok = type_text(text)
            if ok and enter:
                mac_press_key(36)  # Return only when the sentence ends
            self._send_json(200 if ok else 500, {"ok": ok})
            print(f"[agent] /asr typed {len(text)} chars: {text[:40]!r}",
                  file=sys.stderr)
        elif self.path == "/speak":
            ok = forward_speak(text)
            self._send_json(200 if ok else 502, {"ok": ok})
            print(f"[agent] /speak forwarded {len(text)} chars", file=sys.stderr)
        else:
            self._send_json(404, {"ok": False})

    def log_message(self, *a):
        pass  # quiet default logging


def main():
    global ESP_HOST, ESP_PORT
    ap = argparse.ArgumentParser(description="esp32_voice_io PC agent")
    ap.add_argument("--listen", type=int, default=8765,
                    help="HTTP listen port (default 8765)")
    ap.add_argument("--esp", required=True,
                    help="ESP32 address as IP[:port] (port default 8766)")
    args = ap.parse_args()
    if ":" in args.esp:
        ESP_HOST, p = args.esp.split(":", 1)
        ESP_PORT = int(p)
    else:
        ESP_HOST = args.esp
    print(f"[agent] platform={sys.platform} listen=:{args.listen} "
          f"esp={ESP_HOST}:{ESP_PORT}", file=sys.stderr)
    if sys.platform == "darwin":
        print("[agent] grant Accessibility to this terminal "
              "(System Settings > Privacy & Security > Accessibility)",
              file=sys.stderr)
    httpd = ThreadingHTTPServer(("0.0.0.0", args.listen), Handler)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("[agent] stopped", file=sys.stderr)


if __name__ == "__main__":
    main()
