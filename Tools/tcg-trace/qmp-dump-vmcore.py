#!/usr/bin/env python3
# =============================================================================
# qmp-dump-vmcore.py — 通过 QEMU QMP 的官方 dump-guest-memory 抓「vmcore」
#                       （RAM 段 + 全 CPU 寄存器，标准 ELF，GDB 可开）
#
# 为什么用它而不是 pmemsave：
#   pmemsave 只是「物理地址空间的线性窗口复制」——它不枚举 RAM、不生成结构、
#   不含寄存器，洞靠补零、四六不分。dump-guest-memory 是 QEMU 官方 vmcore：
#   * 只枚举真实 RAM 段（GuestPhysBlock），洞跳过 → 紧凑；
#   * 生成标准 ELF（每段一个 PT_LOAD，p_paddr=物理；非 paging 时 p_vaddr=物理）；
#   * 写入【全 CPU】NT_PRSTATUS note（cpu_write_elf64_note）→ GDB 直接 info threads/registers；
#   * 自带 vm_stop（快照一致）并在结束后 vm_start。
#
# 用法:
#   qmp-dump-vmcore.py <qmp-sock> <outfile> [--paging] [--begin A] [--length L]
#                      [--timeout SEC] [--no-quit]
#     <outfile>      .vmcore 输出路径（QEMU 以 file: 协议自建，O_TRUNC）
#     --paging       true：逐 CPU 走客户机页表，p_vaddr=虚拟（慎用，见下）
#     --begin A --length L   ELF filter：只落物理 [A, A+L)（须成对）
#     --timeout SEC  等待 dump 完成的上限（默认 900；到点判失败并删残件）
#     --no-quit      抓完后不 quit（便于继续用该 QEMU 实例）
#
# 产物: <outfile>（物理 vmcore）+ <outfile>.regs（最后寄存器上下文：HMP info registers -a，
#       全 CPU 段基址等；与 core 内 NT_PRSTATUS 互补）
#
# 语义要点（源码 dump/dump.c）:
#   * dump_init: 若 running 则 vm_stop(RUN_STATE_SAVE_VM) → 自带暂停，无需先 stop；
#     dump_cleanup 结束后 vm_start（resume）。
#   * detach=true：dump 在独立线程跑，QMP 立即回；用 query-dump 轮询到 completed/failed。
#   * 非 paging：p_paddr = guest 物理；p_vaddr = 0 → 回落成 p_paddr（即物理地址）。
#     paging：p_vaddr = guest 虚拟（走页表）。⚠ SparrowOS 的 phyaddr_window(10G 别名)
#     会被 paging=true 一并落盘 → 体积暴涨；默认用非 paging。
#   * 只含 RAM：MMIO 洞不落盘（区别于 pmemsave 的「补零窗口」）。
#   * format 缺省/显式 elf；kdump-* 与 paging/filter 互斥。
#
# 退出码: 0=成功  2=dump 失败/超时(已删残件)  3=连接/协议失败  64=用法错误
# =============================================================================
import json
import os
import socket
import sys
import time

CONNECT_TIMEOUT = 10
POLL_INTERVAL = 1.0


def fail(msg, code=3):
    sys.stderr.write("[dump-vmcore] %s\n" % msg)
    sys.exit(code)


def rm(path):
    try:
        os.remove(path)
    except OSError:
        pass


def main():
    argv = sys.argv[1:]
    if len(argv) < 2:
        fail("usage: qmp-dump-vmcore.py <qmp-sock> <outfile> "
             "[--paging] [--begin A --length L] [--timeout SEC] [--no-quit]", 64)
    sock_path, outfile = argv[0], argv[1]
    paging = False
    begin = length = None
    timeout = 900
    do_quit = True
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == "--paging":
            paging = True
        elif a == "--begin":
            i += 1; begin = int(argv[i], 0)
        elif a == "--length":
            i += 1; length = int(argv[i], 0)
        elif a == "--timeout":
            i += 1; timeout = int(argv[i], 0)
        elif a == "--no-quit":
            do_quit = False
        else:
            fail("未知参数: %s" % a, 64)
        i += 1
    if (begin is None) != (length is None):
        fail("--begin 与 --length 必须成对", 64)

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(CONNECT_TIMEOUT)
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

    _id = [0]

    def cmd(execute, arguments=None):
        _id[0] += 1
        msg = {"execute": execute, "id": _id[0]}
        if arguments is not None:
            msg["arguments"] = arguments
        f.write((json.dumps(msg) + "\n").encode())
        f.flush()
        while True:
            o = read_obj()
            if o.get("id") == _id[0]:
                return o

    try:
        read_obj()                            # greeting
        cmd("qmp_capabilities")
        args = {"paging": paging, "protocol": "file:" + outfile,
                "format": "elf", "detach": True}
        if begin is not None:
            args["begin"] = begin
            args["length"] = length
        r = cmd("dump-guest-memory", args)
        if "error" in r:
            fail("dump-guest-memory 报错: %s" % r["error"], 2)

        # 轮询 query-dump 到 completed / failed / 超时
        t0 = time.time()
        last = -1
        while True:
            q = cmd("query-dump")
            st = q.get("return", {})
            status = st.get("status")
            done = st.get("completed", 0)
            total = st.get("total", 0)
            if status == "completed":
                break
            if status == "failed":
                rm(outfile)
                fail("dump 失败(status=failed)", 2)
            if done != last:
                sys.stderr.write("[dump-vmcore] %s %d/%d bytes\n"
                                 % (status, done, total))
                last = done
            if time.time() - t0 > timeout:
                rm(outfile)
                fail("dump 超时 %ds（已删残件）" % timeout, 2)
            time.sleep(POLL_INTERVAL)

        # 结尾补一份「最后的寄存器上下文」（HMP info registers -a：全 CPU 段基址等）
        try:
            rr = cmd("human-monitor-command", {"command-line": "info registers -a"})
            if isinstance(rr, dict) and "return" in rr:
                with open(outfile + ".regs", "w") as g:
                    g.write(rr["return"])
        except Exception:
            pass
    except Exception as e:
        rm(outfile)
        fail("QMP 交互失败: %s" % e, 3)

    if do_quit:
        try:
            cmd("quit")
        except Exception:
            pass

    try:
        sz = os.path.getsize(outfile)
    except OSError:
        sz = -1
    sys.stdout.write("[dump-vmcore] OK paging=%s -> %s (%.1f GB)\n"
                     % (paging, outfile, sz / 2**30))


if __name__ == "__main__":
    main()
