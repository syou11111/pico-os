#!/usr/bin/env python3
"""PicoOS Controller - Native Windows GUI app (BLE auto-connect + Wi-Fi AP toggle)."""

import asyncio
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
from tkinter import messagebox, scrolledtext
import urllib.request

try:
    from bleak import BleakClient, BleakScanner

    HAS_BLEAK = True
except ImportError:
    BleakClient = None
    BleakScanner = None
    HAS_BLEAK = False

DEFAULT_URL = "http://192.168.4.1"
BLE_TARGET_NAME = "PicoOS-BLE"
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

WLAN_PROFILE_XML = """<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>PicoOS-AP</name>
  <SSIDConfig><SSID><name>PicoOS-AP</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>manual</connectionMode>
  <MSM>
    <security>
      <authEncryption>
        <authentication>WPA2PSK</authentication>
        <encryption>AES</encryption>
        <useOneX>false</useOneX>
      </authEncryption>
      <sharedKey>
        <keyType>passPhrase</keyType>
        <protected>false</protected>
        <keyMaterial>picoos123</keyMaterial>
      </sharedKey>
    </security>
  </MSM>
</WLANProfile>
"""

KEYSYM_MAP = {
    "Return": b"\r",
    "KP_Enter": b"\r",
    "BackSpace": b"\x7f",
    "Tab": b"\t",
    "Escape": b"\x1b",
    "Up": b"\x1b[A",
    "Down": b"\x1b[B",
    "Right": b"\x1b[C",
    "Left": b"\x1b[D",
    "Home": b"\x1b[H",
    "End": b"\x1b[F",
    "Delete": b"\x1b[3~",
}


class PicoWifiApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("PicoOS Controller (BLE / Wi-Fi v0.7)")
        self.root.geometry("840x630")
        self.root.configure(bg="#0d1117")

        self.mode = "BLE" if HAS_BLEAK else "WIFI"
        self.base_url = DEFAULT_URL
        self.seq = 0
        self.connected = False
        self.running = True
        self.tx_lock = threading.Lock()
        self.tx_hex_queue = ""
        self.ui_queue: queue.Queue[tuple[str, str]] = queue.Queue()

        self._build_ui()
        self.worker = threading.Thread(target=self._io_loop, daemon=True)
        self.worker.start()
        self.root.after(50, self._drain_ui_queue)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self) -> None:
        top = tk.Frame(self.root, bg="#161b22", padx=10, pady=8)
        top.pack(fill=tk.X)

        tk.Label(
            top, text="PicoOS Controller", fg="#38bdf8", bg="#161b22",
            font=("Segoe UI", 12, "bold")
        ).pack(side=tk.LEFT)

        init_status = "BLEスキャン中..." if self.mode == "BLE" else "Wi-Fi待機中"
        self.status_lbl = tk.Label(
            top, text=init_status, fg="#fde047", bg="#1f242c",
            padx=10, pady=3, font=("Segoe UI", 9, "bold")
        )
        self.status_lbl.pack(side=tk.LEFT, padx=10)

        self.mode_btn = tk.Button(
            top,
            text=f"接続方式: {self.mode}",
            bg="#1d4ed8" if self.mode == "BLE" else "#334155",
            fg="#ffffff",
            relief=tk.FLAT,
            padx=10,
            pady=4,
            command=self._toggle_mode,
        )
        self.mode_btn.pack(side=tk.LEFT, padx=4)

        tk.Button(
            top, text="PicoOS-AP にWi-Fi接続", bg="#0284c7", fg="#ffffff",
            relief=tk.FLAT, padx=10, pady=4, command=self._connect_wifi
        ).pack(side=tk.RIGHT, padx=4)

        self.url_var = tk.StringVar(value=DEFAULT_URL)
        tk.Entry(
            top, textvariable=self.url_var, width=20, bg="#0d1117", fg="#e6edf3",
            insertbackground="#ffffff", relief=tk.SOLID, bd=1
        ).pack(side=tk.RIGHT, padx=4)

        # Terminal output (clicking captures direct keystrokes)
        term_frame = tk.Frame(self.root, bg="#0d1117", padx=10, pady=6)
        term_frame.pack(fill=tk.BOTH, expand=True)

        tk.Label(
            term_frame,
            text="出力画面（ここをクリックして直接キー入力 / 矢印・ESC・Ctrl+C対応）",
            fg="#8b949e", bg="#0d1117", anchor="w", font=("Segoe UI", 9)
        ).pack(fill=tk.X)

        self.term = scrolledtext.ScrolledText(
            term_frame, bg="#05080f", fg="#e2e8f0", insertbackground="#22c55e",
            font=("Consolas", 11), relief=tk.SOLID, bd=1, wrap=tk.WORD
        )
        self.term.pack(fill=tk.BOTH, expand=True, pady=(4, 6))
        self.term.bind("<Key>", self._on_term_key)
        self.term.bind("<Button-1>", lambda e: (self.term.focus_set(), None))

        # Command line bar
        cmd_row = tk.Frame(term_frame, bg="#0d1117")
        cmd_row.pack(fill=tk.X, pady=(0, 6))

        self.cmd_var = tk.StringVar()
        self.cmd_entry = tk.Entry(
            cmd_row, textvariable=self.cmd_var, bg="#161b22", fg="#ffffff",
            insertbackground="#ffffff", font=("Consolas", 11), relief=tk.SOLID, bd=1
        )
        self.cmd_entry.pack(side=tk.LEFT, fill=tk.X, expand=True, ipady=4)
        self.cmd_entry.bind("<Return>", lambda e: self._send_cmd_line())

        tk.Button(
            cmd_row, text="送信 (Enter)", bg="#0284c7", fg="#ffffff",
            relief=tk.FLAT, padx=12, pady=5, command=self._send_cmd_line
        ).pack(side=tk.LEFT, padx=(6, 0))

        # Controls: Hotkeys + D-Pad + Quick Commands
        ctrl_row = tk.Frame(self.root, bg="#161b22", padx=10, pady=8)
        ctrl_row.pack(fill=tk.X)

        left_btns = tk.Frame(ctrl_row, bg="#161b22")
        left_btns.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        r1 = tk.Frame(left_btns, bg="#161b22")
        r1.pack(fill=tk.X, pady=2)
        for label, hx, bg in [
            ("ESC", "1B", "#21262d"),
            ("TAB", "09", "#21262d"),
            ("BS", "7F", "#21262d"),
            ("ENTER", "0D", "#0284c7"),
            ("Ctrl+C", "03", "#991b1b"),
            (":w", "3A770D", "#21262d"),
            (":wq", "3A77710D", "#21262d"),
            (":q!", "3A71210D", "#21262d"),
        ]:
            tk.Button(
                r1, text=label, bg=bg, fg="#ffffff", relief=tk.FLAT,
                padx=9, pady=4, command=lambda h=hx: self._queue_hex(h)
            ).pack(side=tk.LEFT, padx=2)

        r2 = tk.Frame(left_btns, bg="#161b22")
        r2.pack(fill=tk.X, pady=4)
        for label, cmd, bg in [
            ("ps", "ps\r", "#21262d"),
            ("ls", "ls\r", "#21262d"),
            ("ble", "ble\r", "#21262d"),
            ("pinout", "pinout\r", "#21262d"),
            ("disp", "disp\r", "#21262d"),
            ("wifi", "wifi\r", "#21262d"),
            ("clear", "clear\r", "#21262d"),
            ("sensor.c", "cc -r sensor.c\r", "#15803d"),
            ("game.c", "cc -r game.c\r", "#15803d"),
        ]:
            tk.Button(
                r2, text=label, bg=bg, fg="#ffffff", relief=tk.FLAT,
                padx=9, pady=4, command=lambda c=cmd: self._queue_text(c)
            ).pack(side=tk.LEFT, padx=2)

        # D-Pad on right
        dpad = tk.Frame(ctrl_row, bg="#161b22")
        dpad.pack(side=tk.RIGHT, padx=(12, 0))
        tk.Button(
            dpad, text="▲", width=4, bg="#21262d", fg="#ffffff", relief=tk.FLAT,
            command=lambda: self._queue_hex("1B5B41")
        ).grid(row=0, column=1, padx=2, pady=2)
        tk.Button(
            dpad, text="◀", width=4, bg="#21262d", fg="#ffffff", relief=tk.FLAT,
            command=lambda: self._queue_hex("1B5B44")
        ).grid(row=1, column=0, padx=2, pady=2)
        tk.Button(
            dpad, text="▼", width=4, bg="#21262d", fg="#ffffff", relief=tk.FLAT,
            command=lambda: self._queue_hex("1B5B42")
        ).grid(row=1, column=1, padx=2, pady=2)
        tk.Button(
            dpad, text="▶", width=4, bg="#21262d", fg="#ffffff", relief=tk.FLAT,
            command=lambda: self._queue_hex("1B5B43")
        ).grid(row=1, column=2, padx=2, pady=2)

    def _toggle_mode(self) -> None:
        if self.mode == "WIFI":
            if not HAS_BLEAK:
                messagebox.showinfo("BLE未導入", "bleak が未インストールのため Wi-Fi モードのみ利用可能です。")
                return
            self.mode = "BLE"
            self.mode_btn.configure(text="接続方式: BLE", bg="#1d4ed8")
            self.status_lbl.configure(text="BLEスキャン中...", fg="#fde047")
        else:
            self.mode = "WIFI"
            self.mode_btn.configure(text="接続方式: WIFI", bg="#334155")
            self.status_lbl.configure(text="Wi-Fi未接続", fg="#fca5a5")
        self.connected = False

    def _queue_hex(self, hx: str) -> None:
        with self.tx_lock:
            self.tx_hex_queue += hx

    def _queue_text(self, text: str) -> None:
        hx = text.encode("utf-8", errors="ignore").hex().upper()
        self._queue_hex(hx)

    def _send_cmd_line(self) -> None:
        txt = self.cmd_var.get()
        self.cmd_var.set("")
        self._queue_text(txt + "\r")

    def _on_term_key(self, event: tk.Event) -> str:
        if (event.state & 0x4) and event.keysym.lower() == "c":
            self._queue_hex("03")
            return "break"
        if (event.state & 0x4) and event.keysym.lower() == "d":
            self._queue_hex("04")
            return "break"
        if event.keysym in KEYSYM_MAP:
            self._queue_hex(KEYSYM_MAP[event.keysym].hex().upper())
            return "break"
        if event.char and ord(event.char) >= 0x20:
            self._queue_text(event.char)
            return "break"
        return "break"

    def _connect_wifi(self) -> None:
        def worker() -> None:
            try:
                with tempfile.NamedTemporaryFile("w", suffix=".xml", delete=False, encoding="utf-8") as f:
                    f.write(WLAN_PROFILE_XML)
                    xml_path = f.name
                subprocess.run(
                    ["netsh", "wlan", "add", "profile", f"filename={xml_path}", "interface=Wi-Fi"],
                    capture_output=True, text=True, check=False, creationflags=0x08000000
                )
                res = subprocess.run(
                    ["netsh", "wlan", "connect", "name=PicoOS-AP", "interface=Wi-Fi"],
                    capture_output=True, text=True, check=False, creationflags=0x08000000
                )
                if res.returncode == 0:
                    self.ui_queue.put(("status", "Wi-Fi接続要求送信済み..."))
                else:
                    self.ui_queue.put(("status", f"Wi-Fi接続エラー: {res.stderr.strip()}"))
            except Exception as exc:
                self.ui_queue.put(("status", f"Wi-Fiエラー: {exc}"))

        threading.Thread(target=worker, daemon=True).start()

    async def _ble_session(self) -> None:
        self.ui_queue.put(("status", "BLEスキャン中 (PicoOS-BLE)..."))

        def match_pico(dev, adv) -> bool:
            if dev.name == BLE_TARGET_NAME:
                return True
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            return NUS_SERVICE_UUID in uuids

        device = await BleakScanner.find_device_by_filter(match_pico, timeout=4.0)
        if not device or not self.running or self.mode != "BLE":
            return

        self.ui_queue.put(("status", "BLE接続処理中..."))
        async with BleakClient(device) as client:
            def on_notify(_sender: int, data: bytearray) -> None:
                text = data.decode("utf-8", errors="replace")
                body = ANSI_RE.sub("", text).replace("\r", "")
                if body:
                    self.ui_queue.put(("log", body))

            await client.start_notify(NUS_TX_UUID, on_notify)
            self.connected = True
            self.ui_queue.put(("conn", "BLE接続中 (PicoOS-BLE)"))
            while self.running and self.mode == "BLE" and client.is_connected:
                with self.tx_lock:
                    d = self.tx_hex_queue
                    self.tx_hex_queue = ""
                if d:
                    raw = bytes.fromhex(d)
                    for i in range(0, len(raw), 100):
                        await client.write_gatt_char(NUS_RX_UUID, raw[i : i + 100], response=False)
                await asyncio.sleep(0.03)
            self.connected = False
            self.ui_queue.put(("conn", "BLE切断"))

    def _io_loop(self) -> None:
        while self.running:
            if self.mode == "BLE" and HAS_BLEAK:
                try:
                    asyncio.run(self._ble_session())
                except Exception:
                    if self.connected:
                        self.connected = False
                        self.ui_queue.put(("conn", "BLE未接続"))
                time.sleep(0.6)
                continue

            url_base = self.url_var.get().strip().rstrip("/") or DEFAULT_URL
            with self.tx_lock:
                d = self.tx_hex_queue
                self.tx_hex_queue = ""
            try:
                req_url = f"{url_base}/k?s={self.seq}&d={d}"
                with urllib.request.urlopen(req_url, timeout=1.5) as resp:
                    raw = resp.read().decode("utf-8", errors="replace")
                nl = raw.find("\n")
                if nl >= 0:
                    try:
                        self.seq = int(raw[:nl].strip())
                    except ValueError:
                        pass
                    body = ANSI_RE.sub("", raw[nl + 1 :]).replace("\r", "")
                    if body:
                        self.ui_queue.put(("log", body))
                if not self.connected:
                    self.connected = True
                    self.ui_queue.put(("conn", "Wi-Fi接続中 (192.168.4.1)"))
            except Exception:
                if self.connected:
                    self.connected = False
                    self.ui_queue.put(("conn", "Wi-Fi未接続"))
            time.sleep(0.16 if not d else 0.04)

    def _drain_ui_queue(self) -> None:
        while not self.ui_queue.empty():
            kind, val = self.ui_queue.get_nowait()
            if kind == "log":
                self.term.insert(tk.END, val)
                if int(self.term.index("end-1c").split(".")[0]) > 300:
                    self.term.delete("1.0", "100.0")
                self.term.see(tk.END)
            elif kind == "conn":
                ok = "接続中" in val
                self.status_lbl.configure(
                    text=val,
                    fg="#86efac" if ok else "#fca5a5"
                )
            elif kind == "status":
                self.status_lbl.configure(text=val, fg="#fde047")
        if self.running:
            self.root.after(50, self._drain_ui_queue)

    def _on_close(self) -> None:
        self.running = False
        self.root.destroy()


if __name__ == "__main__":
    try:
        root = tk.Tk()
        app = PicoWifiApp(root)
        if "--self-test" in sys.argv:
            root.after(400, app._on_close)
        root.mainloop()
    except Exception as exc:
        try:
            messagebox.showerror("PicoOS Controller Error", str(exc))
        except Exception:
            pass
        sys.exit(1)
