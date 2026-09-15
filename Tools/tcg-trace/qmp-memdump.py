#!/usr/bin/env python3
# =============================================================================
# qmp-memdump.py — 通过 QEMU QMP 抓取「最终物理内存镜像」
#
# 用法:
#   qmp-memdump.py <qmp-sock> <outfile> <base> <size>
#     qmp-sock  QEMU 的 unix QMP socket（-qmp unix:...,server=on,wait=off）
#     outfile   输出文件（原始物理内存镜像，little-endian 直出）
#     base      起始物理地址（支持 0x… 十六进制；常用 0）
#     size      字节数（支持 0x…；常用 全 RAM=0x200000000(8G)）
#
# 行为:
#   连接 → qmp_capabilities → stop（冻结全部 vCPU，得到一致快照）
#        → pmemsave(base,size,outfile) → quit（关掉 QEMU）
#
# 退出码: 0=成功  2=pmemsave 报错  3=连接/协议失败
#
# 说明:
#   * pmemsave 是「同步」命令：返回即表示文件写完（大镜像会等较久）。
#   * 必须在 QEMU 主循环仍活着的窗口调用；storm/panic 下 TCG 主循环独立线程，
#     故 stop/pmemsave 仍可用 —— 这正是把内存镜像和 trace 一起留下的关键。
# =============================================================================
import json
import socket
import sys
import time

def fail(msg, code=3):
    sys.stderr.write("[qmp-memdump] %s\n" % msg)
    sys.exit(code)

def main():
    if len(sys.argv) != 5:
        fail("usage: qmp-memdump.py <qmp-sock> <outfile> <base> <size>", 64)
    sock_path, outfile, base_s, size_s = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    try:
        base = int(base_s, 0)
        size = int(size_s, 0)
    except ValueError:
        fail("base/size 无法解析: %r %r" % (base_s, size_s), 64)

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(30)
    try:
        s.connect(sock_path)
    except OSError as e:
        fail("连接 QMP 失败: %s" % e, 3)
    f = s.makefile("rwb")

    def read_obj():
        while True:
            line = f.readline()
            if not line:
                raise EOFError("QMP 关闭")
            line = line.strip()
            if not line:
                continue
            try:
                return json.loads(line)
            except Exception:
                continue

    def cmd(execute, arguments=None, _id=1):
        msg = {"execute": execute, "id": _id}
        if arguments is not None:
            msg["arguments"] = arguments
        f.write((json.dumps(msg) + "\n").encode())
        f.flush()
        while True:
            o = read_obj()
            if o.get("id") == _id:
                return o
            # 其它是 event，忽略

    try:
        read_obj()                      # greeting
    except Exception as e:
        fail("读取 QMP greeting 失败: %s" % e, 3)

    try:
        cmd("qmp_capabilities", _id=1)
        cmd("stop", _id=2)              # 冻结 vCPU → 一致快照
        t0 = time.time()
        r = cmd("pmemsave", {"val": base, "size": size, "filename": outfile}, _id=3)
        if "error" in r:
            fail("pmemsave 错误: %s" % r["error"], 2)
        cmd("quit", _id=4)
        dt = time.time() - t0
    except Exception as e:
        fail("QMP 交互失败: %s" % e, 3)

    sys.stdout.write("[qmp-memdump] OK %d bytes @0x%x -> %s (%.1fs)\n"
                     % (size, base, outfile, dt))

if __name__ == "__main__":
    main()
