#!/usr/bin/env python3
# ════════════════════════════════════════════════════════════════
# src_loc_decode.py — 源码位置定位码解码器（宿主端，AI 全自动分析管线）
#
# 用法：
#   python3 build_utils/src_loc_decode.py 0x0000123456789ABC
#   python3 build_utils/src_loc_decode.py 1250994816618671996   (十进制)
#
# 流程：
#   1. 解析魔法值 u64
#      line = loc & 0xFFFFF            （[0:19]）
#      hash = loc >> 20                 （[20:63]）
#   2. 读 srcloc.json（hash 升序表）二分查 hash → path
#   3. base_dir + path 拼绝对路径，打印 path:line + 前后 N 行代码片段
# ════════════════════════════════════════════════════════════════
import bisect
import json
import os
import sys

LINE_MASK = 0xFFFFF
CONTEXT_LINES = 6


def locate_srcloc_json():
    """从当前/上级目录向上找 srcloc.json，兼容不同调用工作目录。"""
    here = os.path.abspath(os.path.dirname(os.path.abspath(__file__)))
    for d in (here, os.path.dirname(here), os.getcwd()):
        cand = os.path.join(d, "srcloc.json")
        if os.path.isfile(cand):
            return cand
    return os.path.join(os.path.dirname(here), "srcloc.json")


def decode(raw):
    line = raw & LINE_MASK
    h = raw >> 20
    return h, line


def main():
    if len(sys.argv) < 2:
        print("usage: python3 build_utils/src_loc_decode.py <magic_value>", file=sys.stderr)
        return 2

    arg = sys.argv[1].strip()
    try:
        raw = int(arg, 0)  # 自动识别 0x 前缀 / 十进制
    except ValueError:
        print(f"Error: cannot parse magic value: {arg}", file=sys.stderr)
        return 2

    h, line = decode(raw)

    tbl_path = locate_srcloc_json()
    if not os.path.isfile(tbl_path):
        print(f"Error: srcloc.json not found (looked near {tbl_path})", file=sys.stderr)
        print("Hint: run the build to generate it (src_loc_dumper).", file=sys.stderr)
        return 1

    with open(tbl_path) as f:
        data = json.load(f)

    base_dir = data.get("base_dir", "")
    table = data.get("table", [])
    hashes = [e["hash"] for e in table]

    # 二分：hash 表已升序
    i = bisect.bisect_left(hashes, h)
    if i >= len(hashes) or hashes[i] != h:
        print(f"LOC 0x{raw:x}: line={line} hash=0x{h:x} — NOT FOUND in table", file=sys.stderr)
        return 1

    rel = table[i]["path"]
    abs_path = os.path.join(base_dir, rel) if base_dir else rel

    print(f"{rel}:{line}   (loc=0x{raw:x} hash=0x{h:x})")
    print("-" * 60)

    try:
        with open(abs_path, encoding="utf-8", errors="replace") as f:
            src_lines = f.readlines()
    except FileNotFoundError:
        print(f"[file missing: {abs_path}]", file=sys.stderr)
        return 1

    total = len(src_lines)
    start = max(1, line - CONTEXT_LINES)
    end = min(total, line + CONTEXT_LINES)
    for ln in range(start, end + 1):
        marker = ">>>" if ln == line else "   "
        text = src_lines[ln - 1].rstrip("\n")
        print(f"{marker} {ln:5d} | {text}")
    print("-" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
