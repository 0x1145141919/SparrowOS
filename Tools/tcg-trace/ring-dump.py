#!/usr/bin/env python3
# =============================================================================
# ring-dump.py — 从 .core（虚拟视角）+ kernel.elf 打捞 interrupt_log_ring
#                的日志正文 + 描述元数据；兼作「IRQ-safe 内存日志」发烟验收工具。
#
# 背景：interrupt_log_ring 是 debug_tmp_ring_buff —— 裸文本暂存环，无记录头。
#   它的全部状态 = 一个 DmesgRingBuffer_soul（32B）：
#       { void *buff; uint64_t buffSize;
#         uint64_t accumulate_mileage;      // 自出生累计写入字节（总里程）
#         uint64_t accumulate_record_count; // print 调用条数 }
#   本工具在 core 里：全局指针 → 对象 → soul → 环字节，重建「时间序」文本，
#   并打印元数据（容量/总里程/打印条数/回绕圈数/写游标）。
#   环【无记录分隔】⇒ 只作「最近 N 字节文本」的应急黑匣子，不做记录定界。
#
# 依赖：核心转储（虚拟视角 .core，见 vmcore-mkcore.py）+ 符号表（kernel.elf）。
#   ⚠️ .core 必须【保留恒等窗口别名】（vmcore-mkcore.py 默认即保留）；若用
#      --skip-alias 洗过，环缓冲（经 phyaddr_window 访问）可能不在 core 里 → 报错。
#
# 用法:
#   ring-dump.py <core> [--kernel kernel.elf] [--symbol interrupt_log_ring]
#                [--symbol-addr 0x..] [--max-bytes N] [--out FILE] [--json]
#                [--expect SUBSTR]... [--min-prints N]
#     <core>          虚拟视角 core（vmcore-mkcore.py 产物）
#     --kernel        ELF（解析 --symbol 地址；默认 ./kernel.elf）
#     --symbol        全局环指针符号名（默认 interrupt_log_ring）
#     --symbol-addr   直接给符号 VA，跳过 --kernel 解析（调试/合成用）
#     --max-bytes N   文本最多回显 N 字节（从最新往旧截；默认 65536）
#     --out FILE      把重建的完整文本写到 FILE
#     --json          以 JSON 输出元数据（供脚本消费）
#     --expect S      发烟验收：文本须含子串 S（可多次）；全部命中才 PASS
#     --min-prints N  发烟验收：accumulate_record_count 须 >= N
#
# 退出码: 0 成功(/PASS)  1 验收 FAIL  2 用法错误  3 core 解析失败
#         4 环未初始化/空      5 符号未找到
# =============================================================================
import argparse
import json
import re
import struct
import subprocess
import sys

PT_LOAD = 1
SOUL_SIZE = 32          # void*(8) + u64 + u64 + u64
LOCK_OFF  = SOUL_SIZE   # debug_tmp_ring_buff.working_soul 是首成员；其后为 lock


def die(msg, code=2):
    sys.stderr.write("[ring-dump] %s\n" % msg)
    sys.exit(code)


class Core:
    """极简 ELF64-LE core 读取器：按【虚拟地址】读 PT_LOAD 覆盖的字节。"""

    def __init__(self, path):
        self.f = open(path, "rb")
        d = self.f.read(64)
        if len(d) < 64 or d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            die("不是 ELF64 little-endian core: %s" % path, 3)
        (e_phoff,) = struct.unpack_from("<Q", d, 0x20)
        e_phentsize, e_phnum = struct.unpack_from("<HH", d, 0x36)
        if e_phentsize < 56:
            die("e_phentsize=%d 异常" % e_phentsize, 3)
        self.loads = []   # (vaddr_lo, vaddr_hi, file_off, filesz)
        for i in range(e_phnum):
            self.f.seek(e_phoff + i * e_phentsize)
            ph = self.f.read(e_phentsize)
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
                struct.unpack_from("<IIQQQQQQ", ph, 0)
            if p_type == PT_LOAD and p_filesz:
                self.loads.append((p_vaddr, p_vaddr + p_filesz, p_offset))
        if not self.loads:
            die("core 无 PT_LOAD 段", 3)

    def read(self, va, n):
        """读 n 字节（可跨段）；任一字节不可达返回 None。"""
        out = bytearray()
        while n > 0:
            seg = None
            for lo, hi, off in self.loads:
                if lo <= va < hi:
                    seg = (lo, hi, off)
                    break
            if seg is None:
                return None
            take = min(n, seg[1] - va)
            self.f.seek(seg[2] + (va - seg[0]))
            b = self.f.read(take)
            if len(b) != take:
                return None
            out += b
            va += take
            n -= take
        return bytes(out)

    def u64(self, va):
        b = self.read(va, 8)
        return None if b is None else struct.unpack("<Q", b)[0]

    def is_mapped(self, va, n=1):
        return self.read(va, n) is not None


def find_symbol(elf, name):
    try:
        out = subprocess.run(["nm", "-n", "-C", "--defined-only", elf],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             text=True, errors="replace").stdout
    except OSError:
        die("无法执行 nm（缺 binutils？）", 5)
    for ln in out.splitlines():
        p = ln.split(None, 2)
        if len(p) == 3 and p[2].strip() == name:
            try:
                return int(p[0], 16)
            except ValueError:
                pass
    return None


def render_text(b):
    """字节 → 可读文本：可打印保留，其余（含 NUL/控制符）折成 '.'；\\n/\\t 保留。"""
    out = []
    for c in b:
        if c in (0x0a, 0x09, 0x0d):
            out.append(chr(c))
        elif 0x20 <= c < 0x7f:
            out.append(chr(c))
        else:
            out.append(".")
    return "".join(out)


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("core")
    ap.add_argument("--kernel", default="kernel.elf")
    ap.add_argument("--symbol", default="interrupt_log_ring")
    ap.add_argument("--symbol-addr", default=None)
    ap.add_argument("--max-bytes", type=lambda s: int(s, 0), default=65536)
    ap.add_argument("--out", default=None)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--expect", action="append", default=[])
    ap.add_argument("--min-prints", type=lambda s: int(s, 0), default=None)
    a = ap.parse_args()

    core = Core(a.core)

    # 1) 符号地址
    if a.symbol_addr is not None:
        sym_va = int(a.symbol_addr, 0)
        sym_src = "--symbol-addr"
    else:
        sym_va = find_symbol(a.kernel, a.symbol)
        sym_src = "%s@%s" % (a.symbol, a.kernel)
        if sym_va is None:
            die("符号未找到：%s in %s" % (a.symbol, a.kernel), 5)

    # 2) 全局指针 → 环对象
    p = core.u64(sym_va)
    if p is None:
        die("符号 VA %#x 未映射（core 不含该段？）" % sym_va, 3)
    if p == 0:
        die("interrupt_log_ring == nullptr：环未初始化（该 core 早于落地 / kernel_start 未走到）", 4)

    obj = p
    raw = core.read(obj, LOCK_OFF + 8)
    if raw is None or len(raw) < SOUL_SIZE:
        die("环对象 VA %#x 未映射（vmcore-mkcore 是否 --skip-alias 洗掉了窗口？）" % obj, 3)
    buff, buff_size, mileage, count = struct.unpack_from("<QQQQ", raw, 0)
    lock_status = raw[LOCK_OFF] if len(raw) > LOCK_OFF else None

    if buff == 0 or buff_size == 0:
        die("环 soul 空转（buff=%#x buffSize=%d）：绑定未完成" % (buff, buff_size), 4)

    # 3) 元数据
    wrap = mileage // buff_size
    cursor = mileage % buff_size
    live = min(mileage, buff_size)
    meta = {
        "symbol": a.symbol, "symbol_src": sym_src, "symbol_va": sym_va,
        "ring_obj_va": obj, "buff_va": buff, "buff_size": buff_size,
        "accumulate_mileage": mileage, "accumulate_record_count": count,
        "wrap_rounds": wrap, "write_cursor": cursor, "live_bytes": live,
        "lock_status": lock_status,
    }

    # 4) 重建「时间序」文本（老→新）
    if mileage == 0:
        text_bytes = b""
    else:
        need = buff_size if mileage >= buff_size else mileage
        buf = core.read(buff, need)
        if buf is None:
            die("环缓冲 VA %#x 未映射（.core 缺恒等窗口别名？）" % buff, 3)
        if mileage < buff_size:
            text_bytes = buf[:mileage]
        else:
            text_bytes = buf[cursor:] + buf[:cursor]
    meta["text_bytes"] = len(text_bytes)

    if not a.json:
        print("[ring-dump] core=%s  sym=%s" % (a.core, sym_src))
        print("[ring-dump] ring_obj=0x%x  buff=0x%x  lock=%s" %
              (obj, buff, ("?" if lock_status is None else
                           ("LOCKED" if lock_status else "UNLOCKED"))))
        print("[ring-dump] buffSize=%d  mileage=%d  prints=%d  wrap=%d  cursor=%d  live=%d" %
              (buff_size, mileage, count, wrap, cursor, live))
        print("[ring-dump] --- log text (chronological, oldest→newest; %d B) ---" % len(text_bytes))

    if a.out:
        with open(a.out, "w", errors="replace") as g:
            g.write(render_text(text_bytes))
        sys.stderr.write("[ring-dump] full text -> %s\n" % a.out)

    if not a.json:
        show = text_bytes
        trunc = 0
        if len(show) > a.max_bytes:
            trunc = len(show) - a.max_bytes
            show = show[-a.max_bytes:]
        if trunc:
            print("  ...(%d B 较早内容省略)" % trunc)
        sys.stdout.write(render_text(show))
        if show and not show.endswith(b"\n"):
            print()

    # 5) 发烟验收
    checks = []
    if a.expect:
        for sub in a.expect:
            checks.append(("expect:%s" % sub, sub.encode() in text_bytes))
    if a.min_prints is not None:
        checks.append(("min-prints>=%d" % a.min_prints, count >= a.min_prints))
    verdict = all(ok for _, ok in checks) if checks else None
    if a.json:
        meta["checks"] = {k: v for k, v in checks}
        meta["verdict"] = ("PASS" if verdict else "FAIL") if checks else None
        print(json.dumps(meta, indent=2, ensure_ascii=False))
    if checks:
        for k, ok in checks:
            print("[ring-dump] %-24s %s" % (k, "ok" if ok else "FAIL"))
        print("RING_CHECK=%s" % ("PASS" if verdict else "FAIL"))
        return 0 if verdict else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
