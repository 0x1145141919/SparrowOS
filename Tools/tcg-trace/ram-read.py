#!/usr/bin/env python3
# =============================================================================
# ram-read.py — 从 QEMU `pmemsave` 物理内存镜像里按【虚拟地址】取字节
#
# 背景：tcg-trace.sh --dump-mem 产出的 <tag>.ram 是「物理地址 0 起、小端直出」
#   的内存镜像。内核崩因常表现为「某虚拟地址处的栈/指针被踩」，要落到镜像上，
#   必须先用崩溃时的 CR3 做 x86-64 4 级页表 walk（虚拟→物理），再按物理偏移取。
#
# 用法:
#   ram-read.py <ram> <cr3> <vaddr> [--len N] [--from PA] [--base PA] [--walk] [--ascii]
#     <ram>   镜像文件；<cr3> 崩溃时 CR3（可为 0x…/十进制）；<vaddr> 目标虚拟地址
#     --len N    取 N 字节（默认 64；跨页会停在页边界）
#     --from PA  直接把 PA 当物理地址取字节（跳过 walk）
#     --base PA  镜像起始物理地址（默认 0）
#     --walk     打印 PML4E/PDPTE/PDE/PTE 每一级
#     --ascii    附 ASCII 侧栏（默认只 hex）
# 退出码: 0 成功  3 未映射/越界
# =============================================================================
import sys, argparse, re

PAGEMASK = 0x000ffffffffff000
ASSET_RE = re.compile(r'\[assets_remap\]\s+([^:\s]+)[^\n]*?v=(0x[0-9A-Fa-f]+)\s+p=(0x[0-9A-Fa-f]+)\s+npg=([0-9A-Fa-f]+)')

def parse_assets(path):
    segs = []
    for ln in open(path, 'r', errors='replace'):
        m = ASSET_RE.search(ln)
        if m:
            name, v, p, npg = m.group(1), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)
            segs.append((name, v, p, npg * 0x1000))
    return segs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ram'); ap.add_argument('cr3'); ap.add_argument('vaddr', nargs='?')
    ap.add_argument('--len', '-n', type=lambda s: int(s, 0), default=64)
    ap.add_argument('--from', dest='frm', type=lambda s: int(s, 0))
    ap.add_argument('--base', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--walk', action='store_true')
    ap.add_argument('--ascii', action='store_true')
    ap.add_argument('--assets', help='panic serial；解析其中 [assets_remap] 静态 v→p 表，优先直译')
    ap.add_argument('--list', action='store_true', help='只打印 assets_remap 表')
    a = ap.parse_args()
    cr3 = int(a.cr3, 0); base = a.base
    f = open(a.ram, 'rb')
    segs = parse_assets(a.assets) if a.assets else []
    if a.list:
        for n, v, p, sz in segs:
            print('%-16s v=%#014x p=%#010x size=%#x (%d KiB)' % (n, v, p, sz, sz >> 10))
        return

    def seg_lookup(va):
        for n, v, p, sz in segs:
            if v <= va < v + sz: return p + (va - v), 'assets_remap:%s' % n
        return None, None

    def phys(pa, n):
        if pa is None or pa < base: return None
        f.seek(pa - base); b = f.read(n)
        return b if len(b) == n else None

    def rd64(pa): 
        b = phys(pa, 8); return None if b is None else int.from_bytes(b, 'little')

    def walk(va):
        i4, i3, i2, i1 = (va >> 39) & 0x1ff, (va >> 30) & 0x1ff, (va >> 21) & 0x1ff, (va >> 12) & 0x1ff
        off = va & 0xfff
        e4 = rd64(cr3 + i4*8)
        if not e4 or not (e4 & 1): return None, 'PML4E 空/不存在'
        if a.walk: print('  PML4E[%3d] @ %#x = %#018x' % (i4, cr3+i4*8, e4))
        e3 = rd64((e4 & PAGEMASK) + i3*8)
        if not e3 or not (e3 & 1): return None, 'PDPTE 空'
        if a.walk: print('  PDPTE[%3d] @ %#x = %#018x' % (i3, (e4&PAGEMASK)+i3*8, e3))
        if e3 & (1 << 7):                                   # 1GiB page
            pa = (e3 & 0x000fffffc0000000) + (va & 0x3fffffff)
            return pa, 'PDPTE.PS=1 → 1GiB 页'
        e2 = rd64((e3 & PAGEMASK) + i2*8)
        if not e2 or not (e2 & 1): return None, 'PDE 空'
        if a.walk: print('  PDE  [%3d] @ %#x = %#018x' % (i2, (e3&PAGEMASK)+i2*8, e2))
        if e2 & (1 << 7):                                   # 2MiB page
            pa = (e2 & 0x000fffffffe00000) + (va & 0x1fffff)
            return pa, 'PDE.PS=1 → 2MiB 页'
        e1 = rd64((e2 & PAGEMASK) + i1*8)
        if not e1 or not (e1 & 1): return None, 'PTE 空'
        if a.walk: print('  PTE  [%3d] @ %#x = %#018x' % (i1, (e2&PAGEMASK)+i1*8, e1))
        return (e1 & PAGEMASK) + off, '4KiB 页'

    if a.frm is not None:
        pa, why = a.frm, '--from 指定物理地址'
    else:
        if a.vaddr is None: sys.exit('需要 <vaddr> 或 --from')
        va = int(a.vaddr, 0)
        pa, why = seg_lookup(va)
        if pa is None:
            if a.walk: print('walk vaddr=%#x cr3=%#x:' % (va, cr3))
            pa, why = walk(va)
        if pa is None:
            sys.stderr.write('未映射: %s\n' % why); sys.exit(3)
        va = va
    n = a.len
    if a.vaddr is not None and a.frm is None:
        # 跨页截断
        rem = 0x1000 - (int(a.vaddr, 0) & 0xfff)
        n = min(n, rem)
    b = phys(pa, n)
    if b is None:
        sys.stderr.write('物理读越界: pa=%#x n=%d\n' % (pa, n)); sys.exit(3)
    print('=> pa=%#x (%s) len=%d' % (pa, why, len(b)))
    for i in range(0, len(b), 16):
        chunk = b[i:i+16]
        hexs = ' '.join('%02x' % x for x in chunk)
        line = '%#014x  %-47s' % (pa + i, hexs)
        if a.ascii: line += '  |' + ''.join(chr(x) if 32 <= x < 127 else '.' for x in chunk) + '|'
        print(line)

if __name__ == '__main__':
    main()
