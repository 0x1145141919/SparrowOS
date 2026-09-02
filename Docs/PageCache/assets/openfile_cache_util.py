#!/usr/bin/env python3
# openfile_cache_util.py — 本机所有进程当前打开的文件: 尺寸分布 + 页缓存驻留率(mincore)
# 输出: 打开文件集合的整体利用率、按尺寸桶的驻留字节、<4K 文件的热度、top 驻留文件
import os, stat, ctypes, sys
from collections import defaultdict

PAGE = 4096
libc = ctypes.CDLL(None, use_errno=True)
def page_resident(fd, size, cap=None):
    """mmap + mincore, 返回驻留页数。cap: 最多检查的字节数(取文件头)"""
    if size <= 0: return 0, 0
    n = min(size, cap) if cap else size
    import mmap as _mm
    try:
        m = _mm.mmap(fd, n, access=_mm.ACCESS_COPY)  # 私有只读映射, 不写不 COW; 只为拿地址调 mincore
    except Exception:
        return 0, 0
    np = (n + PAGE - 1) // PAGE
    res = 0
    try:
        base = ctypes.addressof(ctypes.c_char.from_buffer(m))  # 拿 buffer 地址不可靠
    except Exception:
        base = None
    # 用 mmap 对象自身的地址: python mmap 没有直接地址; 用 ctypes 辅助
    # 简化: 逐 128 页调用 mincore, 需要 buffer 指针 —— 通过 numpy? 直接用 ctypes 数组 mmap 太麻烦。
    # 改法: 用 os.mmap 的 buffer_info? 不存在。用 ctypes.CDLL mincore + (ctypes.c_char*n).from_buffer(m)
    buf = (ctypes.c_char * 1).from_buffer(m)
    addr = ctypes.addressof(buf)
    del buf  # 释放指针导出, 否则 m.close() 报 BufferError
    vec = (ctypes.c_ubyte * 128)()
    off = 0
    while off < np:
        chunk = min(128, np - off)
        r = libc.mincore(ctypes.c_void_p(addr + off * PAGE), chunk * PAGE, vec)
        if r != 0:
            e = ctypes.get_errno()
            if e == 12:  # ENOMEM 区域未映射(文件空洞/截断) → 跳过剩余
                break
            # EAGAIN(1): 重试一次
            r = libc.mincore(ctypes.c_void_p(addr + off * PAGE), chunk * PAGE, vec)
            if r != 0: break
        res += sum(1 for i in range(chunk) if vec[i] & 1)
        off += chunk
    m.close()
    return res, np

files = {}   # (dev,ino) -> [path, size, (pid,fd)]
for pid in os.listdir('/proc'):
    if not pid.isdigit(): continue
    fddir = f'/proc/{pid}/fd'
    try: fds = os.listdir(fddir)
    except OSError: continue
    for f in fds:
        try:
            st = os.stat(f'{fddir}/{f}')   # 跟随 fd symlink → 目标文件
        except OSError: continue
        if not stat.S_ISREG(st.st_mode): continue
        try: tgt = os.readlink(f'{fddir}/{f}')
        except OSError: continue
        key = (st.st_dev, st.st_ino)
        if key not in files:
            files[key] = [tgt, st.st_size, (pid, f)]

print(f"当前打开的唯一普通文件数: {len(files)}")
buckets = [(0,4096,'<4K'),(4096,8192,'4-8K'),(8192,16384,'8-16K'),(16384,32768,'16-32K'),
           (32768,65536,'32-64K'),(65536,131072,'64-128K'),(131072,262144,'128-256K'),
           (262144,524288,'256-512K'),(524288,1048576,'512K-1M'),(1048576,4194304,'1-4M'),
           (4194304,16777216,'4-16M'),(16777216,67108864,'16-64M'),(67108864,1<<62,'>64M')]

cnt = defaultdict(int); szb = defaultdict(int); resp = defaultdict(int); rcnt = defaultdict(int)
tot_size = 0; tot_res = 0; res_files = 0; tiny_hot = 0; tiny_hot_bytes = 0
tops = []
for key, (path, size, (pid, f)) in files.items():
    tot_size += size
    rp = np_ = 0
    fd = None
    try: fd = os.open(f'/proc/{pid}/fd/{f}', os.O_RDONLY)
    except OSError:
        try: fd = os.open(path, os.O_RDONLY)
        except OSError: pass
    if fd is not None:
        rp, np_ = page_resident(fd, size)
        os.close(fd)
    rb = rp * PAGE
    tot_res += rb
    if rp: res_files += 1
    for lo, hi, name in buckets:
        if lo <= size < hi:
            cnt[name] += 1; szb[name] += size; resp[name] += rb
            if rp: rcnt[name] += 1
            if size < 4096 and rp: tiny_hot += 1; tiny_hot_bytes += rb
            break
    tops.append((rb, size, path))

print(f"打开文件总体积: {tot_size/1048576:.0f} MB | 驻留页缓存: {tot_res/1048576:.0f} MB | 整体利用率: {100*tot_res/tot_size:.1f}% | 有驻留的文件数: {res_files}")
print(f"\n{'桶':<10}{'文件数':>8}{'字节MB':>10}{'驻留MB':>10}{'有驻留文件':>12}{'桶内驻留率':>10}")
for lo, hi, name in buckets:
    if cnt[name]:
        u = 100*resp[name]/szb[name] if szb[name] else 0
        print(f"{name:<10}{cnt[name]:>8}{szb[name]/1048576:>10.1f}{resp[name]/1048576:>10.1f}{rcnt[name]:>12}{u:>9.1f}%")
print(f"\n<4K 且当前驻留(热)的文件: {tiny_hot} 个, 共占用 {tiny_hot_bytes/1024:.0f} KB 页缓存")
print("\ntop 15 驻留文件 (驻留MB, 尺寸MB, 路径):")
for rb, size, path in sorted(tops, reverse=True)[:15]:
    print(f"  {rb/1048576:8.1f}  {size/1048576:9.1f}  {path[:110]}")
