#!/usr/bin/env python3
# =============================================================================
# trace-sym.py — 把 QEMU `-d in_asm,int,...` 的 .trace 变成「符号+源码行」精读版
#
# 背景：.trace 是裸汇编，且 init.elf / kernel.elf / UEFI 固件三段地址混排；
#   裸读 64 万行不可行。本工具按链接域(ELF)把指令行、异常/中断事件行的
#   地址解析成 `符号+偏移 @ 源文件:行`（DWARF 由 addr2line 提供），并可从
#   「跳入 init.elf」处截断，只精读 OS 本体执行段。
#
# 用法:
#   trace-sym.py <trace> [--init init.elf] [--kernel kernel.elf] [--loader BOOTX64.efi]
#     --start auto|init|kernel|<line>  起始位置（默认 auto＝首个 init.elf 指令行）
#     --regs none|exc|all              事件寄存器 dump 保留策略（默认 exc＝仅异常 v<0x20）
#     --short-loc                      源码位置只留 basename:line
#     --out FILE                       输出（默认 <trace>.sym.log）
#     --summary-only                   只打印符号化后的「异常事件时间线」到 stdout
#
# 说明:
#   * 指令行 = `0xADDR: bytes disasm`；事件行 = `   <cpu>: v=.. e=.. i=.. cpl=.. IP=..:pc ..`
#   * in_asm 是「翻译即记」，同一 TB 只记一次 → 事件行(cpu/向量/寄存器)才是执行序。
#   * 链接域: init.elf @0x101000000；kernel.elf @0xffff800000000000(+低半 0x4000-0x8000)。
# =============================================================================
import sys, os, re, subprocess, argparse, bisect

INIT_BASE = 0x101000000
INIT_LIMIT = 0x102000000          # 宽松上界
KERN_BASE = 0xffff800000000000
KERN_LOW_LO, KERN_LOW_HI = 0x4000, 0x9000   # kernel.elf 低半区 LOAD 段

INST_RE = re.compile(r'^(0x[0-9a-fA-F]+):')
EV_RE = re.compile(r'^\s*(\d+):\s+v=([0-9a-fA-F]+)\s+e=([0-9a-fA-F]+)\s+i=(\d+)\s+'
                   r'cpl=(\d+)\s+IP=([0-9a-fA-F]+):([0-9a-fA-F]+)\s+pc=([0-9a-fA-F]+)')
IMM_RE = re.compile(r'\$(0x[0-9a-fA-F]+|[0-9]+)')

def imm_vals(line):
    """解析指令行里的立即数（十进制或 0x 十六进制）→ int 列表。"""
    out = []
    for m in IMM_RE.finditer(line):
        t = m.group(1)
        try: out.append(int(t, 16) if t[:2].lower() == '0x' else int(t))
        except ValueError: pass
    return out

BYTES_RE = re.compile(r'^0x[0-9a-fA-F]+:\s+((?:[0-9a-fA-F]{2}\s+)+)')
RIP_RE   = re.compile(r'(-?0x[0-9a-fA-F]+)\(%rip\)')

def rip_target(line):
    """RIP-相对位移 → 绝对目标 = 本指令地址 + 指令长度 + 有符号 disp。"""
    m = RIP_RE.search(line)
    if not m: return None
    mb = BYTES_RE.match(line)
    if not mb: return None
    ln = len(mb.group(1).split())          # 该行字节数 = 指令长度（RIP 形式 ≤7 字节，不跨行）
    try: disp = int(m.group(1), 16)
    except ValueError: return None
    a = int(INST_RE.match(line).group(1), 16)
    return a + ln + disp

def domain(a):
    if INIT_BASE <= a < INIT_LIMIT: return 'init'
    if a >= KERN_BASE:              return 'kernel'
    if KERN_LOW_LO <= a < KERN_LOW_HI: return 'kernel'
    return 'other'

def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          text=True, errors='replace', **kw)

def load_nm(elf):
    """sorted list of (addr, name) for defined symbols (for symbol+offset)."""
    out = run(['nm', '-n', '-C', '--defined-only', elf]).stdout
    syms = []
    for ln in out.splitlines():
        p = ln.split(None, 2)
        if len(p) == 3:
            try: syms.append((int(p[0], 16), p[2]))
            except ValueError: pass
    syms.sort()
    return syms

def nearest(syms, a):
    i = bisect.bisect_right(syms, (a, '\xff'))
    if i == 0: return None, 0
    s, n = syms[i-1]
    return n, a - s

def a2l(elf, addrs):
    """addr -> (func, file:line) via addr2line (one batch)."""
    m = {}
    if not addrs: return m
    inp = "".join("0x%x\n" % a for a in addrs)
    out = run(['addr2line', '-f', '-C', '-e', elf], input=inp).stdout.splitlines()
    for i, a in enumerate(addrs):
        f = out[2*i]   if 2*i   < len(out) else '??'
        l = out[2*i+1] if 2*i+1 < len(out) else '??:0'
        m[a] = (f, l)
    return m

def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('trace')
    ap.add_argument('--init', default='/home/PS/PS_git/OS_pj_uefi/kernel/init.elf')
    ap.add_argument('--kernel', default='/home/PS/PS_git/OS_pj_uefi/kernel/kernel.elf')
    ap.add_argument('--start', default='auto')
    ap.add_argument('--only-domain', default=None, choices=['init', 'kernel'],
                    help='只输出该链接域的指令行（事件/寄存器 dump 仍保留）')
    ap.add_argument('--regs', default='exc', choices=['none', 'exc', 'all'])
    ap.add_argument('--short-loc', action='store_true')
    ap.add_argument('--src', action='store_true', help='附加 C/C++ 源码行批注（跳过 .asm/无源码）')
    ap.add_argument('--out', default=None)
    ap.add_argument('--summary-only', action='store_true')
    args = ap.parse_args()

    lines = open(args.trace, 'r', errors='replace').read().splitlines()

    # ---- start line ----
    if args.start == 'auto' or args.start == 'init':
        start = next((i for i, l in enumerate(lines)
                      if (m := INST_RE.match(l)) and domain(int(m.group(1), 16)) == 'init'),
                     len(lines))
    elif args.start == 'kernel':
        start = next((i for i, l in enumerate(lines)
                      if (m := INST_RE.match(l)) and domain(int(m.group(1), 16)) == 'kernel'),
                     len(lines))
    else:
        start = int(args.start)

    # ---- pass 1: collect addresses per domain ----
    addrs = {'init': set(), 'kernel': set()}
    for l in lines[start:]:
        m = INST_RE.match(l)
        if m:
            a = int(m.group(1), 16); d = domain(a)
            if d in addrs: addrs[d].add(a)
            for v in imm_vals(l):
                dv = domain(v)
                if dv in addrs: addrs[dv].add(v)
            rt = rip_target(l)
            if rt is not None:
                dv = domain(rt)
                if dv in addrs: addrs[dv].add(rt)
            continue
        e = EV_RE.match(l)
        if e:
            a = int(e.group(8), 16); d = domain(a)
            if d in addrs: addrs[d].add(a)

    nm_i, nm_k = load_nm(args.init), load_nm(args.kernel)
    loc = {}
    loc.update(a2l(args.init, sorted(addrs['init'])))
    loc.update(a2l(args.kernel, sorted(addrs['kernel'])))

    def tag(addr):
        d = domain(addr)
        if d == 'other': return None
        syms = nm_i if d == 'init' else nm_k
        name, off = nearest(syms, addr)
        name = name if name else '??'
        f, l = loc.get(addr, ('??', '??:0'))
        if args.short_loc and l not in ('??:0',):
            l = l.split('/')[-1]
        return "[%s] %s+0x%x @ %s" % (d[:4], name, off, l)

    # ---- pass 2: emit ----
    if args.summary_only:
        out = None
    else:
        args.out = args.out or (args.trace + '.sym.log')
        out = open(args.out, 'w')
        sys.stderr.write('-> writing %s\n' % args.out)
    def w(s=''):
        if out is not None: out.write(s + '\n')

    events = []          # summary timeline
    prev_dom = None
    i = start
    N = len(lines)

    def resolve_val(v):
        d = domain(v)
        if d not in ('init', 'kernel'): return None
        syms = nm_i if d == 'init' else nm_k
        name, off = nearest(syms, v)
        f, l2 = loc.get(v, ('??', '??:0'))
        if l2 == '??:0' or off > 0x200000: return None   # 无 DWARF 覆盖 = 非常量；偏移过大 = 可疑
        if args.short_loc and l2 not in ('??:0',): l2 = l2.split('/')[-1]
        return '%#x=%s+0x%x@%s' % (v, name or '??', off, l2)

    def addr_suffix(line):
        parts = []
        for v in imm_vals(line):
            r = resolve_val(v)
            if r: parts.append('imm ' + r)
        rt = rip_target(line)
        if rt is not None:
            r = resolve_val(rt)
            if r: parts.append('rip ' + r)
        return ('   ; addr[' + ' | '.join(parts) + ']') if parts else ''

    LOC_RE = re.compile(r'^(.*?):(\d+)')
    src_state = {'last': None}
    src_cache = {}

    def src_line(loc):
        m = LOC_RE.match(loc)
        if not m: return None, None, '??'
        path, ln = m.group(1), int(m.group(2))
        if path not in src_cache:
            try: src_cache[path] = open(path, 'r', errors='replace').read().splitlines()
            except OSError: src_cache[path] = []
        L = src_cache[path]
        return path, ln, (L[ln-1].strip() if 1 <= ln <= len(L) else '??')

    def src_header(a):
        f, l2 = loc.get(a, ('??', '??:0'))
        m = LOC_RE.match(l2)
        if (not m) or m.group(1).lower().endswith(('.asm', '.s')):
            src_state['last'] = None
            return
        if src_state['last'] == (f, l2): return
        src_state['last'] = (f, l2)
        path, ln, text = src_line(l2)
        w('')
        w('  ── %s:%d  [%s]' % (os.path.basename(path), ln, f))
        w('  %6d | %s' % (ln, text))

    while i < N:
        l = lines[i]
        if l.startswith('----------------') or l == 'IN:':
            w(l); i += 1; continue
        m = INST_RE.match(l)
        if m:
            a = int(m.group(1), 16); d = domain(a)
            if d != prev_dom and d in ('init', 'kernel'):
                w('')
                w('##################  JUMP INTO %s.elf  ##################' % d)
                prev_dom = d
            t = tag(a)
            if args.only_domain and d != args.only_domain:
                i += 1; continue
            if args.src: src_header(a)
            w(("%-64s | %s" % (t, l)) + addr_suffix(l) if t else l)
            i += 1; continue
        e = EV_RE.match(l)
        if e:
            cpu, vec, err, irq, cpl, seg, pc, pc2 = e.groups()
            a = int(pc2, 16)
            t = tag(a) or ''
            vecn = int(vec, 16)
            kind = {0x0e: '#PF', 0x0d: '#GP', 0x08: '#DF', 0x00: '#DE', 0x06: '#UD',
                    0x03: '#BP', 0x01: '#DB', 0x0c: '#SS', 0x0a: '#TS', 0x11: '#AC'}.get(vecn, '')
            hu = 'H/W' if vecn >= 0x20 else 'EXC'
            w('')
            w('>>> EVENT #%s %s v=%s%s e=%s i=%s cpl=%s  pc=%#x  %s' %
              (cpu, hu, vec, ('(' + kind + ')') if kind else '', err, irq, cpl, a, t))
            if vecn < 0x20:
                events.append((vecn, cpu, a, t))
            # reg dump: keep per policy
            keep_regs = (args.regs == 'all') or (args.regs == 'exc' and vecn < 0x20)
            i += 1
            while i < N:
                nl = lines[i]
                if nl.startswith('----------------') or nl == 'IN:' or EV_RE.match(nl) \
                   or nl.startswith('Servicing'):
                    break
                if keep_regs: w('    ' + nl)
                i += 1
            continue
        w(l); i += 1

    if not args.summary_only:
        out.flush()
        sys.stderr.write('start_line=%d  init_addrs=%d  kernel_addrs=%d  out=%s\n' %
                         (start+1, len(addrs['init']), len(addrs['kernel']), args.out))
    # timeline to stdout
    sys.stderr.write('---- exception timeline (v<0x20); #N = global event seq ----\n')
    for vec, seq, a, t in events:
        sys.stderr.write('v=%02x #%s pc=%#x %s\n' % (vec, seq, a, t))

if __name__ == '__main__':
    main()
