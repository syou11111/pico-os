#!/usr/bin/env python3
"""PicoOS Wi-Fi Controller - Native Windows GUI app (no browser required)."""

import queue
import re
import subprocess
import tempfile
import threading
import time
import tkinter as tk
from tkinter import messagebox, scrolledtext
import urllib.request

DEFAULT_URL = "http://192.168.4.1"
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
        self.root.title("PicoOS Wi-Fi Controller (v0.6)")
        self.root.geometry("820x620")
        self.root.configure(bg="#0d1117")

        self.base_url = DEFAULT_URL
        self.seq = 0
        self.connected = False
        self.running = True
        self.tx_lock = threading.Lock()
        self.tx_hex_queue = ""
        self.ui_queue: queue.Queue[tuple[str, str]] = queue.Queue()

        self._build_ui()
        self.worker = threading.Thread(target=self._poll_loop, daemon=True)
        self.worker.start()
        self.root.after(50, self._drain_ui_queue)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self) -> None:
        top = tk.Frame(self.root, bg="#161b22", padx=10, pady=8)
        top.pack(fill=tk.X)

        tk.Label(
            top, text="PicoOS Wi-Fi Controller", fg="#38bdf8", bg="#161b22",
            font=("Segoe UI", 12, "bold")
        ).pack(side=tk.LEFT)

        self.status_lbl = tk.Label(
            top, text="待機中", fg="#fde047", bg="#1f242c",
            padx=10, pady=3, font=("Segoe UI", 9, "bold")
        )
        self.status_lbl.pack(side=tk.LEFT, padx=12)

        tk.Button(
            top, text="PicoOS-AP にWi-Fi接続", bg="#0284c7", fg="#ffffff",
            relief=tk.FLAT, padx=10, pady=4, command=self._connect_wifi
        ).pack(side=tk.RIGHT, padx=4)

        self.url_var = tk.StringVar(value=DEFAULT_URL)
        tk.Entry(
            top, textvariable=self.url_var, width=22, bg="#0d1117", fg="#e6edf3",
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

    def _poll_loop(self) -> None:
        while self.running:
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
                    self.ui_queue.put(("conn", "接続中 (192.168.4.1)"))
            except Exception:
                if self.connected:
                    self.connected = False
                    self.ui_queue.put(("conn", "未接続 (PicoOS-AP に繋いでください)"))
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
                ok = val.startswith("接続中")
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
    root = tk.Tk()
    PicoWifiApp(root)
    root.mainloop()
