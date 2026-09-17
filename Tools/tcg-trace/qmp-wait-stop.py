#!/usr/bin/env python3
# qmp-wait-stop.py —— 常驻等待 QEMU 的 STOP 事件（vm_stop / 魔法断点命中）。
#
# 用法: qmp-wait-stop.py <qmp-sock> <flag-file> <timeout-sec>
#   一旦观测到整机被暂停（STOP 事件，或连接时已是 paused/debug）→
#   创建 <flag-file> 并退出 0；超时退出 1；连不上退出 3。
#
# 为什么这样：host 侧要"零轮询开销"地知道 guest 已自停（不能每 0.2s 起一个
# python 去 query-status —— 那本身就是宿主负载，而宿主负载是竞态复现的关键
# 变量）。用一条长连接 + 事件推送，主机侧每轮只需 test -e。
import json, os, socket, sys, time

sock, flag, timeout = sys.argv[1], sys.argv[2], float(sys.argv[3])
deadline = time.time() + timeout
try:
    os.unlink(flag)
except OSError:
    pass

s = None
while time.time() < deadline:
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1.0)
        s.connect(sock)
        break
    except OSError:
        s = None
        time.sleep(0.1)
if s is None:
    sys.exit(3)

def touch():
    open(flag, "w").close()

f = s.makefile("rwb")
s.settimeout(None)

def send(obj):
    f.write((json.dumps(obj) + "\n").encode()); f.flush()

def read_line():
    ln = f.readline()
    if not ln:
        return None
    try:
        return json.loads(ln)
    except Exception:
        return {}

try:
    read_line()                       # greeting
    send({"execute": "qmp_capabilities"})
    while True:
        d = read_line()
        if d is None:
            touch(); sys.exit(0)      # QEMU 没了 → 也算"停了"
        if d.get("event") in ("STOP", "RESET", "SHUTDOWN", "GUEST_PANICKED", "POWERDOWN"):
            touch(); sys.exit(0)
        if "return" in d:             # capabilities 的应答
            send({"execute": "query-status"})
            r = read_line()
            st = (r or {}).get("return", {}).get("status")
            if st in ("paused", "debug", "prelaunch"):
                touch(); sys.exit(0)
        if time.time() >= deadline:
            sys.exit(1)
except (OSError, ValueError):
    touch(); sys.exit(0)
