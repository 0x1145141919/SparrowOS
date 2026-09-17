#!/usr/bin/env python3
# qmp-status.py —— 一次性 query-status，打印 status 字符串（连不上打印 NOCONN）。
# 用途：tcg-trace.sh --selfcheck 校验 QEMU 是否带 ioport80 魔法断点补丁。
import json, socket, sys

sock = sys.argv[1]
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(1.5)
    s.connect(sock)
except OSError:
    print("NOCONN"); sys.exit(3)

f = s.makefile("rwb")

def rd():
    while True:
        ln = f.readline()
        if not ln:
            return None
        try:
            d = json.loads(ln)
        except Exception:
            continue
        if "return" in d or "error" in d:
            return d
    return None

f.readline()  # greeting（含 QMP 版本，无 return/error 字段，不能走 rd）
f.write(b'{"execute":"qmp_capabilities"}\n'); f.flush()
rd()
f.write(b'{"execute":"query-status"}\n'); f.flush()
r = rd()
print(r["return"].get("status", "?") if (r and "return" in r) else "ERR")
