#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""导航状态帧模拟器：发送 / 故障注入 / 回显日志

用途
    M3 阶段用 PC 通过板载 VCP（USART2）向 STM32 灌帧，验证接收端行为。
    不接硬件也能跑 --selftest，先把"组帧是否合规"和"解析器是否能自洽"证明掉。

用法
    python tools/sim_sender.py --demo
        打印样例帧（十六进制），可直接贴进串口助手的 HEX 发送框。

    python tools/sim_sender.py --selftest
        CRC 标准向量 + 8 条样例帧回环 + 故障场景自检。不碰硬件，退出码即结果。

    python tools/sim_sender.py --port COM5 --log
        按 5Hz 发送一条模拟路线，同时把板子回传的字节打印出来（[board] 前缀）。

    python tools/sim_sender.py --port COM5 --fault badcrc --seconds 60
        故障注入，对应验收项 2.4~2.6：
            badcrc  每 10 帧改坏 1 帧的 CRC（应被丢弃，且不影响下一帧）
            half    每 10 帧只发半帧后停顿 100ms（应触发 50ms 超时后恢复）
            glue    每 10 帧把两帧拼在一次 write 里（粘包应被正确拆开）
            noise   每 10 帧前插随机噪声（应被帧头搜索吃掉）
            all     轮换以上四种

依赖
    只有真发串口时才需要 pyserial：pip install pyserial

作者
    <你的名字> · MIT? 按你的仓库许可证来
"""

import argparse
import random
import struct
import sys
import threading
import time

# ---------------------------------------------------------------- 协议常量

HEADER = b"\xA5\x5A"
VER = 0x01
TYPE_STATE = 0x01
FRAME_LEN = 16
TIMEOUT_MS = 50

TURN_STRAIGHT = 0
TURN_LEFT = 1
TURN_RIGHT = 2
TURN_SLIGHT_LEFT = 3
TURN_SLIGHT_RIGHT = 4
TURN_UTURN = 5
TURN_ARRIVE = 6
TURN_UNKNOWN = 7

TURN_NAME = {
    TURN_STRAIGHT: "直行",
    TURN_LEFT: "左转",
    TURN_RIGHT: "右转",
    TURN_SLIGHT_LEFT: "左前",
    TURN_SLIGHT_RIGHT: "右前",
    TURN_UTURN: "掉头",
    TURN_ARRIVE: "到达",
    TURN_UNKNOWN: "未知",
}

FLAG_OFF_ROUTE = 0x01
FLAG_FIX_OK = 0x02
FLAG_MUTE = 0x04

DIST_INVALID = 0xFFFF


def crc16_modbus(data: bytes) -> int:
    """CRC-16/MODBUS：反射多项式 0xA001，初值 0xFFFF，不异或输出。"""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def _clamp16(v: int) -> int:
    return max(0, min(0xFFFF, int(v)))


def build_state(seq, turn, dist_m, speed_kmh, flags, remain_m, eta_min) -> bytes:
    """组一帧 16 字节状态帧。remain_m 由米换算成 10 米单位。"""
    f = bytearray()
    f += HEADER
    f.append(VER)
    f.append(TYPE_STATE)
    f.append(seq & 0xFF)
    f.append(turn & 0xFF)
    f += struct.pack("<H", _clamp16(dist_m))
    f.append(speed_kmh & 0xFF)
    f.append(flags & 0xFF)
    f += struct.pack("<H", _clamp16(remain_m // 10))
    f += struct.pack("<H", _clamp16(eta_min))
    f += struct.pack("<H", crc16_modbus(bytes(f)))
    assert len(f) == FRAME_LEN, f"帧长度错误: {len(f)}"
    return bytes(f)


def hexdump(frame: bytes) -> str:
    return " ".join(f"{b:02X}" for b in frame)


# ------------------------------------------------- 接收端解析器（与 C 端同语义）
# 只为自检而写：证明"我发出去的帧，照我自己的契约一定能解回来"。

class FrameParser:
    def __init__(self):
        self.buf = bytearray()
        self.st = "IDLE"
        self.t_byte = 0
        self.n_ok = 0
        self.n_drop = 0
        self.n_timeout = 0

    def _reset(self):
        self.buf.clear()
        self.st = "IDLE"

    def _resync(self):
        for i in range(1, len(self.buf) - 1):
            if self.buf[i] == 0xA5 and self.buf[i + 1] == 0x5A:
                del self.buf[:i]
                self.st = "RECV"
                return
        if self.buf and self.buf[-1] == 0xA5:
            self.buf = bytearray([0xA5])
            self.st = "WAIT_LSB"
            return
        self._reset()

    def feed(self, byte: int, now_ms: int):
        """返回 dict（解析成功）或 None。语义与 firmware/proto/proto_parser.c 一致。"""
        if self.st != "IDLE" and now_ms - self.t_byte > TIMEOUT_MS:
            self.n_timeout += 1
            self._reset()
        self.t_byte = now_ms

        if self.st == "IDLE":
            if byte == 0xA5:
                self.buf = bytearray([byte])
                self.st = "WAIT_LSB"
            return None

        if self.st == "WAIT_LSB":
            if byte == 0x5A:
                self.buf.append(byte)
                self.st = "RECV"
            elif byte == 0xA5:
                self.buf = bytearray([byte])
            else:
                self._reset()
            return None

        self.buf.append(byte)
        if len(self.buf) < FRAME_LEN:
            return None

        body = bytes(self.buf[:14])
        crc_rx = struct.unpack("<H", bytes(self.buf[14:16]))[0]
        if crc16_modbus(body) != crc_rx or body[2] != VER or body[3] != TYPE_STATE:
            self.n_drop += 1
            self._resync()
            return None

        self.n_ok += 1
        out = {
            "ver": body[2],
            "type": body[3],
            "seq": body[4],
            "turn": body[5],
            "dist_m": struct.unpack("<H", body[6:8])[0],
            "speed_kmh": body[8],
            "flags": body[9],
            "remain_dam": struct.unpack("<H", body[10:12])[0],
            "eta_min": struct.unpack("<H", body[12:14])[0],
            "crc": crc_rx,
        }
        self._reset()
        return out


def parse_stream(data: bytes, step_ms: int = 1, clock=None):
    """把一段字节喂给解析器，返回解析出的帧列表。"""
    p = FrameParser()
    out = []
    t = 0 if clock is None else clock
    for b in data:
        t += step_ms
        r = p.feed(b, t)
        if r:
            out.append(r)
    return out, p


# ---------------------------------------------------------------- 样例帧

def demo_frames():
    """8 条样例帧，覆盖转向枚举 / 长距离 / 标志位。"""
    cases = [
        ("右转 350m, 45km/h, 剩 12.4km", dict(seq=0, turn=TURN_RIGHT, dist_m=350, speed_kmh=45,
                                           flags=FLAG_FIX_OK, remain_m=12400, eta_min=18)),
        ("右转 120m, 38km/h, 剩 12.1km", dict(seq=1, turn=TURN_RIGHT, dist_m=120, speed_kmh=38,
                                           flags=FLAG_FIX_OK, remain_m=12100, eta_min=17)),
        ("左转 800m, 52km/h, 剩 11.5km", dict(seq=2, turn=TURN_LEFT, dist_m=800, speed_kmh=52,
                                           flags=FLAG_FIX_OK, remain_m=11500, eta_min=16)),
        ("掉头 40m, 15km/h, 剩 11.0km", dict(seq=3, turn=TURN_UTURN, dist_m=40, speed_kmh=15,
                                          flags=FLAG_FIX_OK, remain_m=11000, eta_min=16)),
        ("直行 9990m, 60km/h, 剩 9.8km", dict(seq=4, turn=TURN_STRAIGHT, dist_m=9990, speed_kmh=60,
                                            flags=FLAG_FIX_OK, remain_m=9800, eta_min=14)),
        ("到达 30m, 12km/h, 剩 0.2km", dict(seq=5, turn=TURN_ARRIVE, dist_m=30, speed_kmh=12,
                                          flags=FLAG_FIX_OK, remain_m=200, eta_min=1)),
        ("右转 600m + 偏航", dict(seq=6, turn=TURN_RIGHT, dist_m=600, speed_kmh=44,
                              flags=FLAG_OFF_ROUTE | FLAG_FIX_OK, remain_m=9000, eta_min=13)),
        ("左转 250m + 静音", dict(seq=7, turn=TURN_LEFT, dist_m=250, speed_kmh=30,
                              flags=FLAG_MUTE | FLAG_FIX_OK, remain_m=8000, eta_min=12)),
        ("未知转向 + 距离无效", dict(seq=8, turn=TURN_UNKNOWN, dist_m=DIST_INVALID, speed_kmh=0,
                               flags=0, remain_m=DIST_INVALID, eta_min=DIST_INVALID)),
    ]
    return [(label, build_state(**kw)) for label, kw in cases]


def cmd_demo():
    for label, fr in demo_frames():
        print(f"{label}")
        print(f"    {hexdump(fr)}")
    print()
    print("粘包测试用：把任意两行连在一起发（中间不加空格）。")
    print("错 CRC 测试用：把最后一字节按位取反后再发。")


# ---------------------------------------------------------------- 自检

def cmd_selftest() -> int:
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)
            print(f"  FAIL {msg}")

    # 1. CRC 标准向量
    v = crc16_modbus(b"123456789")
    check(v == 0x4B37, f"CRC('123456789') = 0x{v:04X}, 期望 0x4B37")

    # 2. 样例帧长度与回环
    frames = demo_frames()
    check(len(frames) == 9, f"样例帧数量 {len(frames)} != 9")
    for label, fr in frames:
        check(len(fr) == FRAME_LEN, f"[{label}] 帧长 {len(fr)} != 16")
        got, _ = parse_stream(fr)
        check(len(got) == 1, f"[{label}] 回环解析出 {len(got)} 帧，期望 1")
        if got:
            check(got[0]["turn"] == fr[5], f"[{label}] turn 不一致")
            check(fr[14] == got[0]["crc"] & 0xFF, f"[{label}] CRC 低字节顺序错")

    # 3. 粘包
    glue = frames[0][1] + frames[1][1]
    got, p = parse_stream(glue)
    check(len(got) == 2, f"粘包应解出 2 帧，实际 {len(got)}")
    check(p.n_drop == 0, f"粘包不应有丢弃，实际 {p.n_drop}")

    # 4. 坏 CRC 后跟好帧：只丢 1 帧，好帧必须成功
    bad = bytearray(frames[0][1])
    bad[6] ^= 0xFF
    got, p = parse_stream(bytes(bad) + frames[1][1])
    check(len(got) == 1, f"坏帧+好帧应解出 1 帧，实际 {len(got)}")
    if got:
        check(got[0]["seq"] == 1, f"好帧 seq 应为 1，实际 {got[0]['seq']}")

    # 5. 半帧超时
    fr = frames[0][1]
    p = FrameParser()
    t = 0
    for b in fr[:10]:
        t += 1
        p.feed(b, t)
    t += 60
    p.feed(0x00, t)          # 喂一个字节触发超时检查
    for b in fr[10:]:
        t += 1
        p.feed(b, t)
    check(p.n_ok == 0, f"超时后不应解出帧，实际 {p.n_ok}")
    check(p.n_timeout >= 1, f"应记录超时，实际 {p.n_timeout}")

    # 6. 噪声前缀
    got, _ = parse_stream(bytes([0xFF, 0x00, 0x13]) + fr)
    check(len(got) == 1, f"噪声前缀后应解出 1 帧，实际 {len(got)}")

    # 7. 5Hz 连发 200 帧
    p = FrameParser()
    t = 0
    n_ok = 0
    for i in range(200):
        f = build_state(i & 0xFF, TURN_SLIGHT_RIGHT, 1000 - i, 47, FLAG_FIX_OK, 12000, 18)
        for b in f:
            t += 1
            if p.feed(b, t):
                n_ok += 1
        t += 200
    check(n_ok == 200, f"连发 200 帧应全部解出，实际 {n_ok}")

    if fails:
        print(f"\n  FAILED: {len(fails)} 项")
        return 1
    print("\nALL PASS（sim_sender 自检通过）")
    return 0


# ---------------------------------------------------------------- 发送

ROUTE = [
    (TURN_RIGHT, 800),
    (TURN_LEFT, 600),
    (TURN_SLIGHT_RIGHT, 400),
    (TURN_UTURN, 150),
    (TURN_ARRIVE, 60),
]

_FAULTS = ("none", "badcrc", "half", "glue", "noise", "all")


class _Reader(threading.Thread):
    """把板子回传的字节打印出来，避免再开一个串口助手抢 COM 口。"""

    def __init__(self, ser, stop_evt):
        super().__init__(daemon=True)
        self.ser = ser
        self.stop_evt = stop_evt
        self.buf = b""

    def run(self):
        while not self.stop_evt.is_set():
            try:
                chunk = self.ser.read(self.ser.in_waiting or 1)
            except Exception:
                break
            if not chunk:
                continue
            self.buf += chunk
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                print(f"\n[board] {line.decode('utf-8', 'replace').rstrip()}", end="")


def _corrupt(frame: bytes) -> bytes:
    f = bytearray(frame)
    f[6] ^= 0xFF
    return bytes(f)


def _noise_prefix() -> bytes:
    return bytes(random.randrange(0, 0x100) for _ in range(random.randrange(1, 5)))


def cmd_send(args) -> int:
    try:
        import serial
    except ImportError:
        print("缺少 pyserial：pip install pyserial", file=sys.stderr)
        return 2

    period = 1.0 / args.hz
    ser = serial.Serial(args.port, args.baud, timeout=0.05)
    stop_evt = threading.Event()
    if args.log:
        _Reader(ser, stop_evt).start()

    print(f"已打开 {args.port} @ {args.baud}，{args.hz}Hz，fault={args.fault}")
    print("Ctrl+C 停止（停止即模拟断连，板子应在 3 秒后提示）")

    seq = 0
    sent = 0
    t_start = time.time()
    faults = ["badcrc", "half", "glue", "noise"] if args.fault == "all" else [args.fault]

    try:
        while True:
            for turn, start in ROUTE:
                dist = start
                while dist > 0:
                    remain_m = max(0, 12000 - seq * 20)
                    eta = max(0, 18 - seq // 20)
                    fr = build_state(seq, turn, dist, 45, FLAG_FIX_OK, remain_m, eta)
                    fault = "none" if args.fault == "none" else faults[(sent // 10) % len(faults)]

                    if fault == "badcrc":
                        ser.write(_corrupt(fr))
                    elif fault == "half":
                        ser.write(fr[:8])
                        time.sleep(0.1)      # >50ms，触发接收端超时
                        ser.write(fr[8:])
                    elif fault == "glue":
                        nxt = build_state((seq + 1) & 0xFF, turn, max(0, dist - 10),
                                          45, FLAG_FIX_OK, remain_m, eta)
                        ser.write(fr + nxt)
                    elif fault == "noise":
                        ser.write(_noise_prefix() + fr)
                    else:
                        ser.write(fr)

                    print(f"\rseq={seq & 0xFF:<4} {TURN_NAME[turn]:<4} dist={dist:<5}m "
                          f"fault={fault:<7} sent={sent}", end="")
                    seq += 1
                    sent += 1
                    dist -= max(10, dist // 8)

                    if args.seconds and (time.time() - t_start) >= args.seconds:
                        raise KeyboardInterrupt
                    time.sleep(period)
    except KeyboardInterrupt:
        print(f"\n已停止，共发送 {sent} 帧")
    finally:
        stop_evt.set()
        ser.close()
    return 0


# ---------------------------------------------------------------- 入口

def main() -> int:
    ap = argparse.ArgumentParser(description="导航状态帧模拟器（发送/故障注入/回显）")
    ap.add_argument("--demo", action="store_true", help="打印样例帧，不开串口")
    ap.add_argument("--selftest", action="store_true", help="CRC 与协议自检，不开串口")
    ap.add_argument("--port", help="串口名，如 COM5")
    ap.add_argument("--baud", type=int, default=9600, help="默认 9600（DX-BT24 出厂值）")
    ap.add_argument("--hz", type=float, default=5.0, help="发送频率，默认 5Hz")
    ap.add_argument("--log", action="store_true", help="同时打印板子回传的日志")
    ap.add_argument("--fault", choices=_FAULTS, default="none", help="故障注入模式")
    ap.add_argument("--seconds", type=float, help="发送多少秒后自动停止")
    args = ap.parse_args()

    if args.selftest:
        return cmd_selftest()
    if args.demo:
        cmd_demo()
        return 0
    if not args.port:
        ap.print_help()
        return 1
    return cmd_send(args)


if __name__ == "__main__":
    sys.exit(main())
