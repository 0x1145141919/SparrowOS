#!/usr/bin/env python3
# =============================================================================
# vmcore-mkcore.py — 从官方 .vmcore(物理) 合成「虚拟视角」GDB core
#                    （单根 kspace_up_half + 健全性闸；寄存器沿用 vmcore 的 note）
#
# 为什么这么做：
#   QEMU dump-guest-memory(non-paging) 给的是【物理】ELF + 全 CPU NT_PRSTATUS，
#   干净、可靠，但 GDB 按虚拟地址读不到内核栈/堆。SparrowOS 的内核半段(高 128TB)
#   有一张【扁平 PDPTE 表】kspace_up_half[256*512] 作为权威根：
#       index = (vaddr - 0xFFFF800000000000) >> 30      (17 位，1GiB 粒度)
#   于是自造核心 = 以该表为根走 PDPTE→PD→PT，把「虚拟→物理」写成 PT_LOAD
#   (p_vaddr=虚拟, p_offset=PA_BASE+物理)，数据用稀疏文件承载；NOTE 段原样搬运。
#   文件布局： [0,PA_BASE)=ehdr/段表/NOTE； [PA_BASE,..)=物理内存窗口(稀疏)；
#   所有段(内核虚拟视图 + 恒等窗口)共用这一个物理窗口。
#
#   相比老 ram-mkcore.py：根不同（kspace_up_half，不再从 CR3 走），且加了
#   【健全性闸】——页表项解析出的物理帧若不在 vmcore 的 RAM 段内，判为可疑，
#   只报告不落盘（旧版会盲写 -> 一个野 PTE 就能把稀疏文件顶到 768GiB）。
#
# 用法:
#   vmcore-mkcore.py <vmcore> <serial> [--kernel KERNEL_ELF] [--out CORE]
#                    [--pdpt-phys 0x..] [--skip-alias] [--with-phys]
#                    [--report FILE] [--max-suspicious N]
#     <vmcore>   dump-guest-memory 产物(ELF, 物理)
#     <serial>   同次 panic 串口（取 assets_remap：kernel_bss 等 v->p）
#     --kernel   解析符号 kspace_up_half（默认 ./kernel.elf）
#     --pdpt-phys 直接给 kspace_up_half 物理基址（跳过符号/资产推导）
#     --skip-alias    跳过 assets 里名为 phyaddr_window 的整段 PA 恒等别名
#                     （默认【保留】——很多资产如内存元数据(fpa_bitmaps/pages_arr)
#                      经恒等窗口访问；代价是 core 会胀到≈全 RAM）
#     --with-phys     额外为已映射帧加 p_vaddr=物理 的 LOAD（同物理地址也能读）
#     --report F      把「可疑页表项」清单写 F
#
# 输出: <out> (默认 <vmcore>.core)  —— gdb kernel.elf <out>
# 退出码: 0 成功  2 参数/输入问题  3 无法定位 pdpt
# =============================================================================
import argparse, mmap, os, re, struct, subprocess, sys

PAGEMASK = 0x000ffffffffff000
MASK_1G  = 0x000fffffc0000000
MASK_2M  = 0x000fffffffe00000
KSPACE_BASE = 0xFFFF800000000000
N_PDPT = 256 * 512                      # 131072 个 1GiB 槽 = 128TiB
PT_LOAD, PT_NOTE = 1, 4

ASSET_RE = re.compile(r'\[assets_remap\]\s+([^:\s]+)[^\n]*?v=(0x[0-9A-Fa-f]+)\s+'
                      r'p=(0x[0-9A-Fa-f]+)\s+npg=([0-9A-Fa-f]+)')
SEG_RE = re.compile(r'\]\s+(0x[0-9A-Fa-f]+)\s+\+(0x[0-9A-Fa-f]+)\s+(\w+)')
RAM_TYPES = {'free', 'ACPI_NVS', 'ACPI_RECLM', 'ACPI_DATA'}
# 内核 DEFAULT_PAT_CONFIG=0x0407050600070106 解出的 idx->缓存策略
CACHE_NAMES = {0: 'WB', 1: 'WC', 2: 'UC-', 3: 'UC', 4: 'WP', 5: 'WB', 6: 'UC-', 7: 'WT'}
UNCACHE = {2, 3, 6}          # UC / UC-  = 不可缓存(设备/MMIO 信号)


def cache_idx(e, size):
    pat = (e >> (7 if size == 0x1000 else 12)) & 1   # 4K:bit7  2M/1G:bit12
    return (pat << 2) | (((e >> 4) & 1) << 1) | ((e >> 3) & 1)


def parse_segments(path):
    segs = []
    for ln in open(path, 'r', errors='replace'):
        m = SEG_RE.search(ln)
        if m:
            b = int(m.group(1), 16)
            segs.append((b, b + int(m.group(2), 16), m.group(3)))
    return segs


def parse_assets(path):
    segs = []
    for ln in open(path, 'r', errors='replace'):
        m = ASSET_RE.search(ln)
        if m:
            segs.append((m.group(1), int(m.group(2), 16),
                         int(m.group(3), 16), int(m.group(4), 16) * 0x1000))
    return segs


def v2p_assets(segs, v):
    for _, vb, pb, sz in segs:
        if vb <= v < vb + sz:
            return pb + (v - vb)
    return None


def sym_vaddr(kernel, name):
    out = subprocess.run(['nm', '-n', kernel], capture_output=True, text=True)
    for ln in out.stdout.splitlines():
        parts = ln.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('vmcore'); ap.add_argument('serial')
    ap.add_argument('--kernel', default='kernel.elf')
    ap.add_argument('--out')
    ap.add_argument('--pdpt-phys', default=None)
    ap.add_argument('--skip-alias', action='store_true',
                    help='跳过 phyaddr_window 恒等别名（默认保留）')
    ap.add_argument('--with-phys', action='store_true')
    ap.add_argument('--report', default=None)
    ap.add_argument('--baseline', default=None,
                    help='健康态跑出的 report；其中的非 RAM 映射视为预期，不再报异常')
    ap.add_argument('--max-suspicious', type=int, default=40)
    a = ap.parse_args()
    out = a.out or (a.vmcore + '.core')
    segs = parse_assets(a.serial)

    # ── 1. 定位 kspace_up_half 物理基址 ────────────────────────────────
    if a.pdpt_phys:
        pdpt_phys = int(a.pdpt_phys, 0)
    else:
        v = sym_vaddr(a.kernel, 'kspace_up_half')
        if v is None:
            sys.exit("[mkcore] 在 %s 里找不到符号 kspace_up_half（用 --pdpt-phys）" % a.kernel)
        pdpt_phys = v2p_assets(segs, v)
        if pdpt_phys is None:
            sys.exit("[mkcore] kspace_up_half v=%#x 不在 serial assets 内（用 --pdpt-phys）" % v)
    print("[mkcore] kspace_up_half phys = %#x" % pdpt_phys)

    # ── 2. 解析 vmcore 的 RAM 段(物理真值) 与 NOTE ─────────────────────
    f = open(a.vmcore, 'rb'); mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
    eh = struct.unpack_from('<16sHHIQQQIHHHHHH', mm, 0)
    phoff, phentsize, phnum = eh[5], eh[9], eh[10]
    phdrs = []
    for i in range(phnum):
        phdrs.append(struct.unpack_from('<IIQQQQQQ', mm, phoff + i * phentsize))
    ram = [(p[3], p[3] + p[5], p[2]) for p in phdrs if p[0] == PT_LOAD]   # (start,end,foff)
    note = next((p for p in phdrs if p[0] == PT_NOTE), None)
    note_bytes = mm[note[2]:note[2] + note[5]] if note else b''
    ram.sort()

    def ram_ok(pa):
        for s, e, _ in ram:
            if s <= pa < e:
                return True
        return False

    def paddr_bytes(pa, n):
        for s, e, fo in ram:
            if s <= pa and pa + n <= e:
                return mm[fo + (pa - s): fo + (pa - s) + n]
        return None

    def rd64(pa):
        b = paddr_bytes(pa, 8)
        return None if b is None else int.from_bytes(b, 'little')

    # ── 3. 从 kspace_up_half 走内核半段 ────────────────────────────────
    skip_alias = []
    if a.skip_alias:
        for nm, vb, pb, sz in segs:
            if 'phyaddr_window' in nm:
                skip_alias.append((vb, vb + sz))
    def aliased(v):
        return any(s <= v < e for s, e in skip_alias)

    pmsegs = parse_segments(a.serial)
    def classify(pa):
        """帧不在 RAM 段时的性质: MMIO(设备/保留) vs ANOMALY(真异常)."""
        for s, e, t in pmsegs:
            if s <= pa < e:
                return 'MMIO' if t not in RAM_TYPES else 'ANOMALY'
        return 'ANOMALY'          # 不在任何已知物理段 = 越界野值

    baseline = set()
    if a.baseline:
        for ln in open(a.baseline, errors='replace'):
            if ln.startswith('#'):
                continue
            t = ln.split()
            if t:
                baseline.add(int(t[0], 16))

    regions = []            # (v, p, size, kind)
    mmio = []               # 合法 MMIO/保留映射(无 RAM 数据, 仅记录)
    known = []              # 基线已见(预期哨兵/占位)
    anomalies = []          # 真异常
    unc_ram = []            # phys 在 RAM 内却标 UC(罕见,提示)
    stats = {'pdpte': 0, 'pdpte_1g': 0, 'pde_2m': 0, 'pte_4k': 0,
             'note_present': 0, 'skipped': 0,
             'n_mmio': 0, 'n_known': 0, 'n_anom': 0, 'n_unc_ram': 0}

    def emit(v, p, size, kind, e):
        ci = cache_idx(e, size)
        kind = '%s/%s' % (kind, CACHE_NAMES.get(ci, '?'))
        if aliased(v):
            stats['skipped'] += 1
            return
        uncache = ci in UNCACHE
        if not ram_ok(p):
            stats['skipped'] += 1
            if uncache or classify(p) == 'MMIO':
                stats['n_mmio'] += 1
                if len(mmio) < a.max_suspicious:
                    mmio.append((v, p, size, kind))
            elif v in baseline:
                stats['n_known'] += 1
                if len(known) < a.max_suspicious:
                    known.append((v, p, size, kind))
            else:
                stats['n_anom'] += 1
                if len(anomalies) < a.max_suspicious:
                    anomalies.append((v, p, size, kind))
            return
        if uncache:          # phys 在 RAM 内但 UC 标记不可缓存：可疑，单列
            stats['n_unc_ram'] += 1
            if len(unc_ram) < a.max_suspicious:
                unc_ram.append((v, p, size, kind))
        regions.append((v, p, size, kind))

    for i in range(N_PDPT):
        e = rd64(pdpt_phys + i * 8)
        if e is None:
            break
        if not (e & 1):
            continue
        stats['pdpte'] += 1
        v = KSPACE_BASE + (i << 30)
        if e & 0x80:
            stats['pdpte_1g'] += 1
            emit(v, e & MASK_1G, 1 << 30, '1G', e)
            continue
        pd = e & PAGEMASK
        for j in range(512):
            de = rd64(pd + j * 8)
            if not de or not (de & 1):
                continue
            v2 = v + (j << 21)
            if de & 0x80:
                stats['pde_2m'] += 1
                emit(v2, de & MASK_2M, 1 << 21, '2M', de)
            else:
                pt = de & PAGEMASK
                for k in range(512):
                    te = rd64(pt + k * 8)
                    if te and (te & 1):
                        stats['pte_4k'] += 1
                        emit(v2 + (k << 12), te & PAGEMASK, 0x1000, '4K', te)

    # ── 4. 合并连续段（v 与 p 同时连续）────────────────────────────────
    regions.sort(key=lambda r: r[0])
    merged = []
    for v, p, sz, kind in regions:
        if merged and v == merged[-1][0] + merged[-1][2] and p == merged[-1][1] + merged[-1][2]:
            mv, mp, msz, mk = merged[-1]
            merged[-1] = (mv, mp, msz + sz, mk)
        else:
            merged.append((v, p, sz, kind))
    print("[mkcore] walk: pdpte=%d(1G=%d) pde2M=%d pte4K=%d | regions=%d -> merged=%d | skipped=%d"
          % (stats['pdpte'], stats['pdpte_1g'], stats['pde_2m'], stats['pte_4k'],
             len(regions), len(merged), stats['skipped']))

    # ── 5. 布局：文件头/段表/NOTE 在前；其后为【物理内存窗口】基座 ──────
    #    [0, PA_BASE)        = ehdr + 段表 + NOTE(寄存器)
    #    [PA_BASE, PA_BASE+PA)= 物理内存窗口（稀疏；file offset = PA_BASE + phys）
    #    所有 LOAD（内核虚拟视图 + 恒等窗口）都共用这一个物理窗口。
    loads = list(merged)
    if a.with_phys:
        for v, p, sz, kind in merged:
            loads.append((p, p, sz, 'RAM'))          # 物理别名 LOAD
    phnum_out = len(loads) + (1 if note_bytes else 0)
    phoff_out = 0x40
    hdr_end = phoff_out + phnum_out * 56
    note_off = (hdr_end + 7) & ~7
    note_end = note_off + len(note_bytes)
    PA_BASE = (note_end + 0xfff) & ~0xfff            # 物理窗口文件基址
    if PA_BASE < 0x1000:
        PA_BASE = 0x1000

    core = open(out, 'wb'); core.truncate(0)
    written = set(); used = 0; missing = 0
    for _, p, sz, _ in merged:
        for pa in range(p & ~0xfff, p + sz, 0x1000):
            if pa in written:
                continue
            written.add(pa)
            b = paddr_bytes(pa, 0x1000)
            if b is None:
                missing += 1
                continue
            core.seek(PA_BASE + pa); core.write(b); used += 0x1000

    # 头/段表/NOTE 写在前面（不再覆盖物理 0..）
    core.seek(0)
    ident = b'\x7fELF' + bytes([2, 1, 1, 0]) + b'\x00' * 8
    core.write(ident)
    core.write(struct.pack('<HHIQQQIHHHHHH', 4, 62, 1, 0, phoff_out, 0, 0,
                           0x40, 56, phnum_out, 64, 0, 0))
    core.seek(phoff_out)
    for v, p, sz, _ in loads:
        core.write(struct.pack('<IIQQQQQQ', PT_LOAD, 7, PA_BASE + p, v, p, sz, sz, 0x1000))
    if note_bytes:
        core.write(struct.pack('<IIQQQQQQ', PT_NOTE, 4, note_off, 0, 0,
                               len(note_bytes), len(note_bytes), 4))
        core.seek(note_off); core.write(note_bytes)
    maxoff = PA_BASE + max((p + sz for _, p, sz, _ in merged), default=0)
    core.truncate(max(maxoff, note_end))
    core.close()

    print("[mkcore] core=%s LOAD=%d NOTE=%dB data≈%.1fMB missing_pages=%d PA_BASE=%#x"
          % (out, len(loads), len(note_bytes), used / 1048576.0, missing, PA_BASE))
    print("[mkcore] 非 RAM 映射: MMIO/设备=%d(合法,UC+保留)  基线已知=%d  真异常=%d"
          % (stats['n_mmio'], stats['n_known'], stats['n_anom']))
    if stats['n_unc_ram']:
        print("[mkcore] 提示: %d 条 phys 在 RAM 内却标不可缓存(UC/UC-)" % stats['n_unc_ram'])
    if anomalies:
        print("[mkcore] ⚠ 真异常页表项（物理帧越界/不在任何物理段——疑似被踩）:")
        for v, p, sz, kind in anomalies[:12]:
            print("          v=%#014x phys=%#012x size=%#x (%s)" % (v, p, sz, kind))
    if a.report:
        with open(a.report, 'w') as g:
            g.write("# 真异常页表项（物理帧越界/不在任何物理段）\n# vaddr phys size kind\n")
            for v, p, sz, kind in anomalies:
                g.write("%#x %#x %#x %s\n" % (v, p, sz, kind))
            g.write("\n# 基线已知（预期哨兵/占位）\n# vaddr phys size kind\n")
            for v, p, sz, kind in known:
                g.write("%#x %#x %#x %s\n" % (v, p, sz, kind))
            g.write("\n# MMIO/设备（合法，无 RAM 数据）\n# vaddr phys size kind\n")
            for v, p, sz, kind in mmio:
                g.write("%#x %#x %#x %s\n" % (v, p, sz, kind))
            if unc_ram:
                g.write("\n# phys 在 RAM 内却标不可缓存(UC/UC-)\n# vaddr phys size kind\n")
                for v, p, sz, kind in unc_ram:
                    g.write("%#x %#x %#x %s\n" % (v, p, sz, kind))
        print("[mkcore] report -> %s" % a.report)


if __name__ == '__main__':
    main()
