#!/usr/bin/env python3
# =============================================================================
# ram-mkcore.py — 由 .ram(物理镜像) + serial(assets_remap/CR3) 合成 GDB 可读的
#                ELF ET_CORE：把「虚拟地址→物理偏移」写成 PT_LOAD，让 GDB 直接
#                按内核虚拟地址读内存（符号/反汇编/结构体打印全可用）。
#
# 为什么需要：SparrowOS 跑在 0xffff8000… 虚拟空间，而 pmemsave 出的是物理镜像；
#   直接 gdb kernel.elf <raw.ram> 读不了。这里用 assets_remap 静态表 + x86-64 页表
#   walk 枚举映射，生成 core；数据用**稀疏文件**承载（p_offset=物理地址），同物理页
#   只落一次盘，故实体占用 ≈ 被映射到的物理页之和（排除 phyaddr_window 这类巨别名）。
#
# 用法:
#   ram-mkcore.py <ram> <serial> [--out CORE] [--cr3 0x…] [--no-walk]
#                 [--exclude REGEX] [--max-mb N] [--gdb-out FILE]
#   --no-walk        只用 assets_remap 静态表（不枚举页表；动态栈将不可读）
#   --exclude RE     跳过名字匹配 RE 的段（默认排除 phyaddr_window 巨别名）
#   --max-mb N       落盘数据上限（默认 1024MB，超出报警截断）
#   --gdb-out F      额外生成 GDB 初始化脚本（从 serial 抄寄存器）
# =============================================================================
import sys, argparse, re, struct, os

PAGEMASK = 0x000ffffffffff000
ASSET_RE = re.compile(r'\[assets_remap\]\s+([^:\s]+)[^\n]*?v=(0x[0-9A-Fa-f]+)\s+p=(0x[0-9A-Fa-f]+)\s+npg=([0-9A-Fa-f]+)')
CR3_RE   = re.compile(r'CR3\s*[:=]\s*(?:0x)?([0-9A-Fa-f]+)')
REG_RE   = re.compile(r'^\s*([A-Z][A-Z0-9_]*)\s*[:=]\s*0x([0-9A-Fa-f]+)', re.M)

def canon(v):
    return v | 0xffff000000000000 if (v >> 47) & 1 else v

def parse_assets(path):
    segs = []
    for ln in open(path, 'r', errors='replace'):
        m = ASSET_RE.search(ln)
        if m:
            n = m.group(1)
            segs.append((n, int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16) * 0x1000))
    return segs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ram'); ap.add_argument('serial')
    ap.add_argument('--out'); ap.add_argument('--cr3', default=None)
    ap.add_argument('--no-walk', action='store_true')
    ap.add_argument('--exclude', default='phyaddr_window')
    ap.add_argument('--max-mb', type=int, default=1024)
    ap.add_argument('--gdb-out', default=None)
    a = ap.parse_args()
    out = a.out or (a.ram + '.core')

    stxt = open(a.serial, 'r', errors='replace').read()
    cr3 = int(a.cr3, 0) if a.cr3 else (int(CR3_RE.search(stxt).group(1), 16) if CR3_RE.search(stxt) else None)

    f = open(a.ram, 'rb')
    def rd64(pa):
        f.seek(pa); b = f.read(8)
        return int.from_bytes(b, 'little') if len(b) == 8 else None

    regions = []   # (name, vaddr, phys, size)
    exre = re.compile(a.exclude) if a.exclude else None
    for n, v, p, sz in parse_assets(a.serial):
        if exre and exre.search(n): 
            print('  [skip] %s (excluded)' % n); continue
        regions.append((n, v, p, sz))

    if not a.no_walk:
        if cr3 is None:
            sys.exit('需要 CR3（serial 里没有，且未给 --cr3）')
        print('  page-walk from cr3=%#x …' % cr3)
        n_reg = 0
        for i4 in range(512):
            e4 = rd64(cr3 + i4*8)
            if not e4 or not (e4 & 1): continue
            for i3 in range(512):
                e3 = rd64((e4 & PAGEMASK) + i3*8)
                if not e3 or not (e3 & 1): continue
                if e3 & (1 << 7):
                    regions.append(('walk1G', canon((i4<<39)|(i3<<30)), e3 & 0x000fffffc0000000, 1<<30)); n_reg += 1; continue
                for i2 in range(512):
                    e2 = rd64((e3 & PAGEMASK) + i2*8)
                    if not e2 or not (e2 & 1): continue
                    if e2 & (1 << 7):
                        regions.append(('walk2M', canon((i4<<39)|(i3<<30)|(i2<<21)), e2 & 0x000fffffffe00000, 1<<21)); n_reg += 1; continue
                    base = canon((i4<<39)|(i3<<30)|(i2<<21))
                    for i1 in range(512):
                        e1 = rd64((e2 & PAGEMASK) + i1*8)
                        if e1 and (e1 & 1):
                            regions.append(('walk4K', base|(i1<<12), e1 & PAGEMASK, 1<<12)); n_reg += 1
        print('  walk regions=%d' % n_reg)

    # merge contiguous (va+size==next.va and pa+size==next.pa), dedup phys pages
    regions.sort(key=lambda r: (r[1], r[2]))
    merged = []
    for n, v, p, sz in regions:
        if merged and v == merged[-1][1] + merged[-1][3] and p == merged[-1][2] + merged[-1][3]:
            mn, mv, mp, ms = merged[-1]; merged[-1] = (mn, mv, mp, ms + sz)
        else:
            merged.append((n, v, p, sz))
    print('  merged LOAD regions=%d' % len(merged))

    # write sparse core: data at p_offset == physical address; header last
    written_pages = set()
    budget = a.max_mb * 1024 * 1024
    used = 0
    core = open(out, 'wb')
    core.truncate(0)
    for n, v, p, sz in merged:
        for pa in range(p & ~0xfff, p + sz, 0x1000):
            if pa in written_pages: continue
            written_pages.add(pa)
            if used + 0x1000 > budget:
                print('  ! budget %dMB hit; stop writing' % a.max_mb); break
            f.seek(pa); b = f.read(0x1000)
            if len(b) < 0x1000: b = b + b'\x00' * (0x1000 - len(b))
            core.seek(pa); core.write(b); used += 0x1000
        if used + 0x1000 > budget: break
    print('  data written ≈ %.1f MB (%d pages)' % (used/1048576.0, len(written_pages)))

    # 寄存器 NOTE（NT_PRSTATUS）——让 GDB 有 $rip/$rsp/$regs（bt/x/i 都活）
    regs = {m.group(1).lower(): int(m.group(2), 16) for m in REG_RE.finditer(stxt)}
    if 'rflags' in regs: regs['eflags'] = regs['rflags']
    order = ['r15','r14','r13','r12','rbp','rbx','r11','r10','r9','r8','rax','rcx','rdx','rsi',
             'rdi','orig_rax','rip','cs','eflags','rsp','ss','fs_base','gs_base','ds','es','fs','gs']
    pr = bytearray(336)
    struct.pack_into('<27Q', pr, 112, *[regs.get(k, 0) for k in order])
    struct.pack_into('<i', pr, 32, 1)   # pr_pid
    note_name = b'CORE\x00'
    note = (struct.pack('<III', len(note_name), len(pr), 1) + note_name
            + b'\x00' * ((-len(note_name)) % 4) + bytes(pr))

    maxoff = max(p + sz for _, _, p, sz in merged)
    note_off = (maxoff + 3) & ~3
    phnum = len(merged) + 1
    phoff = 0x40; ehsize, phentsize = 0x40, 56
    ident = b'\x7fELF' + bytes([2, 1, 1, 0]) + b'\x00'*8
    core.seek(0); core.write(ident)
    core.write(struct.pack('<HHIQQQIHHHHHH', 4, 62, 1, 0, phoff, 0, 0, ehsize, phentsize, phnum, 64, 0, 0))
    core.seek(phoff)
    for n, v, p, sz in merged:
        core.write(struct.pack('<IIQQQQQQ', 1, 7, p, v, v, sz, sz, 0x1000))
    core.write(struct.pack('<IIQQQQQQ', 4, 4, note_off, 0, 0, len(note), len(note), 4))  # PT_NOTE
    core.seek(note_off); core.write(note)
    core.truncate(note_off + len(note))
    core.close()
    print('=> core: %s  (LOAD=%d, hi_vaddr=%#x, size=%.2f GB)' %
          (out, phnum, max(r[1]+r[3] for r in merged), os.path.getsize(out)/2**30))

    if a.gdb_out:
        regs = {m.group(1): int(m.group(2), 16) for m in REG_RE.finditer(stxt)}
        lines = ['set pagination off', 'set architecture i386:x86-64']
        for k, g in [('RIP','rip'),('RSP','rsp'),('RBP','rbp'),('RFLAGS','eflags'),
                     ('RAX','rax'),('RBX','rbx'),('RCX','rcx'),('RDX','rdx'),('RSI','rsi'),
                     ('RDI','rdi'),('R8','r8'),('R9','r9'),('R10','r10'),('R11','r11'),
                     ('R12','r12'),('R13','r13'),('R14','r14'),('R15','r15')]:
            if k in regs: lines.append('set $%s = 0x%x' % (g, regs[k]))
        if 'CR3' in regs: lines.append('set $cr3 = 0x%x' % regs['CR3'])
        if 'GS_BASE' in regs: lines.append('set $gs_base = 0x%x' % regs['GS_BASE'])
        lines += ['echo \\n-- regs loaded from serial; try:  x/8i $rip  |  info symbol $rip  |  x/32gx $rsp --\\n']
        open(a.gdb_out, 'w').write('\n'.join(lines) + '\n')
        print('=> gdb init: %s' % a.gdb_out)

if __name__ == '__main__':
    main()
