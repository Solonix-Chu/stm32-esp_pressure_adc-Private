#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import queue
import re
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Deque, Dict, Iterable, List, Optional, Tuple
from urllib.parse import urlparse

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


@dataclass(frozen=True)
class Sample:
    t_wall: float
    quat: Optional[Tuple[float, float, float, float]] = None
    roll: Optional[float] = None
    pitch: Optional[float] = None
    yaw: Optional[float] = None
    turn_roll: Optional[float] = None
    turn_pitch: Optional[float] = None
    turn_yaw: Optional[float] = None
    temp_c: Optional[float] = None
    ts_us: Optional[int] = None
    raw_a: Optional[Tuple[int, int, int]] = None
    raw_g: Optional[Tuple[int, int, int]] = None
    a_g: Optional[Tuple[float, float, float]] = None
    g_dps: Optional[Tuple[float, float, float]] = None
    a_nav_g: Optional[Tuple[float, float, float]] = None
    lin_a_nav_g: Optional[Tuple[float, float, float]] = None
    gyro_nav_dps: Optional[Tuple[float, float, float]] = None
    shake: Optional[Tuple[float, float, float]] = None
    rot: Optional[Tuple[float, float, float]] = None

    def to_dict(self) -> Dict:
        return {
            "t": self.t_wall,
            "quat": list(self.quat) if self.quat is not None else None,
            "roll": self.roll,
            "pitch": self.pitch,
            "yaw": self.yaw,
            "turn_roll": self.turn_roll,
            "turn_pitch": self.turn_pitch,
            "turn_yaw": self.turn_yaw,
            "temp_c": self.temp_c,
            "ts_us": self.ts_us,
            "raw_a": self.raw_a,
            "raw_g": self.raw_g,
            "a_g": self.a_g,
            "g_dps": self.g_dps,
            "a_nav_g": self.a_nav_g,
            "lin_a_nav_g": self.lin_a_nav_g,
            "gyro_nav_dps": self.gyro_nav_dps,
            "shake": self.shake,
            "rot": self.rot,
        }


class LogParser:
    def __init__(self) -> None:
        self._re_quat = re.compile(
            r"quat=\[\s*([\-0-9.]+)\s+([\-0-9.]+)\s+([\-0-9.]+)\s+([\-0-9.]+)\s*\]"
        )
        self._re_euler = re.compile(
            r"euler_deg=\[\s*([\-0-9.]+)\s+([\-0-9.]+)\s+([\-0-9.]+)\s*\]"
        )
        self._re_turn = re.compile(
            r"turn_deg=\[\s*([\-0-9.]+)\s+([\-0-9.]+)\s+([\-0-9.]+)\s*\]"
        )
        self._re_temp = re.compile(r"\bt=\s*([\-0-9.]+)\s*C\b")
        self._re_ts = re.compile(r"\bts=\s*([0-9]+)\b")
        self._re_raw_a = re.compile(r"raw_a=\[\s*([\-0-9]+)\s+([\-0-9]+)\s+([\-0-9]+)\s*\]")
        self._re_raw_g = re.compile(r"raw_g=\[\s*([\-0-9]+)\s+([\-0-9]+)\s+([\-0-9]+)\s*\]")
        self._re_a_g = re.compile(r"\ba_g=\[\s*([\-0-9.]+)\s+([\-0-9.]+)\s+([\-0-9.]+)\s*\]")
        self._re_g_dps = re.compile(r"\bg_dps=\[\s*([\-0-9.]+)\s+([\-0-9.]+)\s+([\-0-9.]+)\s*\]")
        self._re_a_nav_g = re.compile(r"\ba_nav_g=\[\s*([\-0-9.]+)\s+([\-0-9.]+)\s+([\-0-9.]+)\s*\]")
        self._re_lin_a_nav_g = re.compile(r"\blin_a_nav_g=\[\s*([\-0-9.]+)\s+([\-0-9.]+)\s+([\-0-9.]+)\s*\]")
        self._re_gyro_nav_dps = re.compile(
            r"\bgyro_nav_dps=\[\s*([\-0-9.]+)\s+([\-0-9.]+)\s+([\-0-9.]+)\s*\]"
        )
        self._re_shake = re.compile(r"\bshake=\[\s*([\-0-9.]+)\s+([\-0-9.]+)\s+([\-0-9.]+)\s*\]")
        self._re_rot = re.compile(r"\brot=\[\s*([\-0-9.]+)\s+([\-0-9.]+)\s+([\-0-9.]+)\s*\]")

    def feed_line(self, line: str) -> Optional[Sample]:
        line = ANSI_RE.sub("", line).strip()
        if not line:
            return None

        quat = None
        roll = None
        pitch = None
        yaw = None

        mq = self._re_quat.search(line)
        if mq:
            quat = (float(mq.group(1)), float(mq.group(2)), float(mq.group(3)), float(mq.group(4)))
        m = self._re_euler.search(line)
        if m:
            roll = float(m.group(1))
            pitch = float(m.group(2))
            yaw = float(m.group(3))

        if quat is None and m is None:
            return None

        turn_roll = None
        turn_pitch = None
        turn_yaw = None
        mtur = self._re_turn.search(line)
        if mtur:
            try:
                turn_roll = float(mtur.group(1))
                turn_pitch = float(mtur.group(2))
                turn_yaw = float(mtur.group(3))
            except ValueError:
                turn_roll = None
                turn_pitch = None
                turn_yaw = None

        temp_c = None
        ts_us = None
        raw_a = None
        raw_g = None
        a_g = None
        g_dps = None
        a_nav_g = None
        lin_a_nav_g = None
        gyro_nav_dps = None
        shake = None
        rot = None

        mt = self._re_temp.search(line)
        if mt:
            try:
                temp_c = float(mt.group(1))
            except ValueError:
                temp_c = None

        mts = self._re_ts.search(line)
        if mts:
            try:
                ts_us = int(mts.group(1))
            except ValueError:
                ts_us = None

        mra = self._re_raw_a.search(line)
        if mra:
            raw_a = (int(mra.group(1)), int(mra.group(2)), int(mra.group(3)))

        mrg = self._re_raw_g.search(line)
        if mrg:
            raw_g = (int(mrg.group(1)), int(mrg.group(2)), int(mrg.group(3)))

        mag = self._re_a_g.search(line)
        if mag:
            a_g = (float(mag.group(1)), float(mag.group(2)), float(mag.group(3)))

        mgd = self._re_g_dps.search(line)
        if mgd:
            g_dps = (float(mgd.group(1)), float(mgd.group(2)), float(mgd.group(3)))

        man = self._re_a_nav_g.search(line)
        if man:
            a_nav_g = (float(man.group(1)), float(man.group(2)), float(man.group(3)))

        mln = self._re_lin_a_nav_g.search(line)
        if mln:
            lin_a_nav_g = (float(mln.group(1)), float(mln.group(2)), float(mln.group(3)))

        mgn = self._re_gyro_nav_dps.search(line)
        if mgn:
            gyro_nav_dps = (float(mgn.group(1)), float(mgn.group(2)), float(mgn.group(3)))

        msh = self._re_shake.search(line)
        if msh:
            shake = (float(msh.group(1)), float(msh.group(2)), float(msh.group(3)))

        mrt = self._re_rot.search(line)
        if mrt:
            rot = (float(mrt.group(1)), float(mrt.group(2)), float(mrt.group(3)))

        return Sample(
            t_wall=time.time(),
            quat=quat,
            roll=roll,
            pitch=pitch,
            yaw=yaw,
            turn_roll=turn_roll,
            turn_pitch=turn_pitch,
            turn_yaw=turn_yaw,
            temp_c=temp_c,
            ts_us=ts_us,
            raw_a=raw_a,
            raw_g=raw_g,
            a_g=a_g,
            g_dps=g_dps,
            a_nav_g=a_nav_g,
            lin_a_nav_g=lin_a_nav_g,
            gyro_nav_dps=gyro_nav_dps,
            shake=shake,
            rot=rot,
        )


class Hub:
    def __init__(self, history_size: int = 2000) -> None:
        self._clients: List["queue.Queue[Dict]"] = []
        self._lock = threading.Lock()
        self._history: Deque[Dict] = deque(maxlen=history_size)

    def add_client(self) -> "queue.Queue[Dict]":
        q: "queue.Queue[Dict]" = queue.Queue(maxsize=200)
        with self._lock:
            self._clients.append(q)
        return q

    def remove_client(self, q: "queue.Queue[Dict]") -> None:
        with self._lock:
            try:
                self._clients.remove(q)
            except ValueError:
                pass

    def history(self) -> List[Dict]:
        with self._lock:
            return list(self._history)

    def publish(self, sample: Dict) -> None:
        with self._lock:
            self._history.append(sample)
            clients = list(self._clients)

        for q in clients:
            try:
                q.put_nowait(sample)
            except queue.Full:
                try:
                    _ = q.get_nowait()
                except queue.Empty:
                    pass
                try:
                    q.put_nowait(sample)
                except queue.Full:
                    pass


def build_http_server(static_dir: str, hub: Hub, host: str, port: int) -> ThreadingHTTPServer:
    static_dir_abs = os.path.abspath(static_dir)

    class Handler(BaseHTTPRequestHandler):
        server_version = "imu_viz/1.0"

        def _send_json(self, obj: object, status: int = 200) -> None:
            data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _serve_static(self, path: str) -> None:
            if path in ("/", ""):
                path = "/index.html"
            rel = path.lstrip("/")
            full = os.path.abspath(os.path.join(static_dir_abs, rel))
            if not full.startswith(static_dir_abs + os.sep) and full != static_dir_abs:
                self.send_error(HTTPStatus.FORBIDDEN)
                return
            if not os.path.exists(full) or not os.path.isfile(full):
                self.send_error(HTTPStatus.NOT_FOUND)
                return

            ctype = "application/octet-stream"
            if full.endswith(".html"):
                ctype = "text/html; charset=utf-8"
            elif full.endswith(".js"):
                ctype = "application/javascript; charset=utf-8"
            elif full.endswith(".css"):
                ctype = "text/css; charset=utf-8"
            elif full.endswith(".svg"):
                ctype = "image/svg+xml"

            with open(full, "rb") as f:
                data = f.read()
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self) -> None:  # noqa: N802
            path_only = urlparse(self.path).path
            if path_only == "/history":
                self._send_json(hub.history())
                return
            if path_only == "/events":
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "text/event-stream; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Connection", "keep-alive")
                self.end_headers()

                q = hub.add_client()
                try:
                    snapshot = hub.history()[-500:]
                    self.wfile.write(b"event: snapshot\n")
                    self.wfile.write(("data: " + json.dumps(snapshot, ensure_ascii=False) + "\n\n").encode("utf-8"))
                    self.wfile.flush()

                    while True:
                        try:
                            item = q.get(timeout=1.0)
                            payload = json.dumps(item, ensure_ascii=False)
                            self.wfile.write(("data: " + payload + "\n\n").encode("utf-8"))
                            self.wfile.flush()
                        except queue.Empty:
                            self.wfile.write(b": ping\n\n")
                            self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                finally:
                    hub.remove_client(q)
                return

            self._serve_static(path_only)

        def log_message(self, fmt: str, *args) -> None:
            # Silence default HTTP logs; console is for ESP parsing logs.
            return

    httpd = ThreadingHTTPServer((host, port), Handler)
    return httpd


def iter_stdin_lines() -> Iterable[str]:
    for line in sys.stdin:
        yield line


def iter_file_lines(path: str, follow: bool) -> Iterable[str]:
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        if follow:
            f.seek(0, os.SEEK_END)
        while True:
            line = f.readline()
            if line:
                yield line
                continue
            if not follow:
                break
            time.sleep(0.05)


def iter_serial_lines(port: str, baud: int) -> Iterable[str]:
    try:
        import serial  # type: ignore
    except Exception as e:
        raise RuntimeError("pyserial not available; install it or use --stdin/--file") from e

    ser = serial.Serial(port=port, baudrate=baud, timeout=0.2)
    try:
        buf = b""
        while True:
            chunk = ser.read(1024)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                yield (line + b"\n").decode("utf-8", errors="ignore")
    finally:
        ser.close()


def main() -> int:
    ap = argparse.ArgumentParser(description="Parse ESP-IDF logs and visualize IMU Euler angles in browser (no deps).")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--stdin", action="store_true", help="Read logs from stdin")
    src.add_argument("--file", type=str, help="Read logs from a file")
    src.add_argument("--serial", type=str, help="Read logs from a serial port (requires pyserial)")

    ap.add_argument("--follow", action="store_true", help="Follow file like tail -f (with --file)")
    ap.add_argument("--baud", type=int, default=115200, help="Serial baudrate (with --serial)")

    ap.add_argument("--http", type=str, default="127.0.0.1", help="HTTP bind host")
    ap.add_argument("--port", type=int, default=8008, help="HTTP bind port")
    ap.add_argument("--static", type=str, default=os.path.join(os.path.dirname(__file__), "static"), help="Static dir")
    ap.add_argument("--history", type=int, default=2000, help="History size")
    ap.add_argument("--max-hz", type=float, default=30.0, help="Max publish rate to UI")

    args = ap.parse_args()

    parser = LogParser()
    hub = Hub(history_size=args.history)

    httpd = build_http_server(args.static, hub, args.http, args.port)

    last_pub = 0.0
    min_interval = 0.0 if args.max_hz <= 0 else (1.0 / args.max_hz)

    def ingest(lines: Iterable[str]) -> None:
        nonlocal last_pub
        for line in lines:
            s = parser.feed_line(line)
            if not s:
                continue
            now = time.monotonic()
            if min_interval > 0 and (now - last_pub) < min_interval:
                continue
            last_pub = now
            hub.publish(s.to_dict())

    if args.stdin:
        line_iter = iter_stdin_lines()
    elif args.file:
        line_iter = iter_file_lines(args.file, follow=args.follow)
    else:
        line_iter = iter_serial_lines(args.serial, baud=args.baud)

    t_ingest = threading.Thread(target=ingest, args=(line_iter,), daemon=True)
    t_ingest.start()

    print(f"[imu_viz] Serving UI on http://{args.http}:{args.port}/", file=sys.stderr)
    try:
        httpd.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        return 0
    finally:
        httpd.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
