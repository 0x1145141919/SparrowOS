#!/usr/bin/env python3
# =============================================================================
# wraith-stackcheck.py -- post-halt stack checker (GDB embedded Python).
#
# Purpose: WRAITH acceptance. After the guest freezes (TEST_FREEZE "#TB#" or an
# assertion / magic breakpoint), run these checks on the halt snapshot
# (.core virtual view) or a live gdbstub:
#   (1) INV-A double-scheduling canary: no task may be registered on two cores
#       in g_cpu_running[] at once;
#   (2) per test thread (g_wraith_slots[]): state / kernel-stack range / saved
#       context (iret) / stack canary self-consistency.
#
# Usage (via stackcheck.sh or manually):
#   gdb -q -batch -ex "source wraith-stackcheck.py" -ex "wraith-stackcheck" \
#       kernel.elf core
#   live: gdb -q kernel.elf -ex "target remote :1234" \
#         -ex "source wraith-stackcheck.py" -ex "wraith-stackcheck"
#
# Deps: kernel.elf with DWARF (Debug) and built with -DKTHREAD_TEST_SCENARIO
# (otherwise only check (1) runs). Last line: [sc] VERDICT=PASS|FAIL.
# NOTE: keep this file ASCII-only -- gdb's Python stdout is often ASCII.
# =============================================================================
import gdb


def _u(expr):
    return int(gdb.parse_and_eval(expr))


class WraithStackCheck(gdb.Command):
    """post-halt stack check: double-scheduling canary + test-thread stack canary"""

    def __init__(self):
        super().__init__("wraith-stackcheck", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        inf = gdb.selected_inferior()

        def rd(addr, n=8):
            return inf.read_memory(int(addr), n).tobytes()

        def u64(addr):
            return int.from_bytes(rd(addr, 8), "little")

        def is_kva(p):
            return p != 0 and (p >> 48) == 0xFFFF

        def state_of(t):
            try:
                return int(t["task_state"])
            except Exception:
                return int(t["task_state"].cast(gdb.lookup_type("uint8_t")))

        state_names = {0: "init", 1: "ready", 2: "running", 3: "blocked",
                       4: "zombie", 5: "dead"}

        # -- cpu count + g_cpu_running[] ------------------------------------
        try:
            ncpu = _u("g_sched_ncpu")
        except gdb.error:
            ncpu = 0
        if ncpu <= 0:
            try:
                ncpu = _u("logical_processor_count")
            except gdb.error:
                ncpu = 1
        print("[sc] ncpu=%d" % ncpu)

        running = []
        for c in range(ncpu):
            try:
                running.append(_u("g_cpu_running[%d]" % c))
            except gdb.error:
                running.append(0)
        print("[sc] g_cpu_running = " +
              ", ".join("cpu%d=%s" % (c, ("%#x" % p) if p else "0")
                        for c, p in enumerate(running)))

        seen = {}
        for c, p in enumerate(running):
            if p:
                seen.setdefault(p, []).append(c)
        dup = [(p, cs) for p, cs in seen.items() if len(cs) > 1]
        if dup:
            for p, cs in dup:
                print("[sc] !!! INV-A VIOLATION: task %#x registered on cpus %s "
                      "(double-scheduling!)" % (p, cs))
        else:
            print("[sc] INV-A ok: no task registered on >1 cpu")

        # -- test-thread registry (only with -DKTHREAD_TEST_SCENARIO) --------
        try:
            nslots = _u("g_wraith_slot_count")
        except gdb.error:
            print("[sc] g_wraith_slots absent -- kernel not built with "
                  "-DKTHREAD_TEST_SCENARIO; only double-scheduling check done.")
            print("[sc] VERDICT=%s" % ("FAIL" if dup else "PASS"))
            return

        print("[sc] test slots=%d" % nslots)
        task_t = gdb.lookup_type("task").pointer()
        bad = 0
        for i in range(int(nslots)):
            base = "g_wraith_slots[%d]" % i
            try:
                if not _u(base + ".in_use"):
                    continue
                tid = _u(base + ".tid")
                role = _u(base + ".role")
                tptr = _u(base + ".task_ptr")
                cadr = _u(base + ".canary_addr")
                cexp = _u(base + ".canary_expected")
                seq = _u(base + ".seq")
            except gdb.error:
                break
            if not is_kva(tptr):
                print("[sc] FAIL slot%2d tid=%d task_ptr=%#x not a kernel pointer"
                      % (i, tid, tptr))
                bad += 1
                continue
            t = gdb.Value(tptr).cast(task_t).dereference()
            st = state_of(t)
            sb = int(t["priv_stack_base"])
            spg = int(t["priv_stack_pages"])
            bel = int(t["belonged_processor_id"])
            wp = int(t["wake_pending"])
            iret = t["priv_ctx"]["core_ctx"]["idtctx"]["iret"]
            irip = int(iret["rip"]); irsp = int(iret["rsp"]); ics = int(iret["cs"])
            on = [c for c in range(ncpu) if running[c] == tptr]

            errs = []
            note = None
            if is_kva(sb) and spg:
                lo, hi = sb, sb + (spg << 12)
                if not (lo <= irsp < hi):
                    errs.append("iret.rsp %#x outside stack [%#x,%#x)" % (irsp, lo, hi))
            else:
                errs.append("bad stack base=%#x pages=%d" % (sb, spg))
            # Registration vs state: the ONLY hard violation is a double
            # registration (INV-A). The F4 handoff design has legitimate
            # one-step transients (published state vs still-registered core),
            # so those are reported as notes, not failures.
            if len(on) > 1:
                errs.append("registered on cpus %s (double-scheduling!)" % on)
            elif st == 2 and not on:
                note = "running, pre-register window (not yet in g_cpu_running[])"
            elif st != 2 and len(on) == 1:
                note = ("handoff window: %s but still registered on cpu%d"
                        % (state_names.get(st, st), on[0]))
            canary_ok = None
            try:
                cv = u64(cadr)
                canary_ok = (cv == cexp)
            except gdb.error:
                canary_ok = None
            if canary_ok is False and st in (1, 2, 3):
                errs.append("stack canary corrupt @%#x got=%#x want=%#x"
                            % (cadr, cv, cexp))

            if errs:
                bad += 1
            tag = "OK  " if not errs else "FAIL"
            extra = ("  | " + "; ".join(errs)) if errs else ""
            if note:
                extra += "  [note: %s]" % note
            print("[sc] %s slot%2d tid=%-4d role=%d state=%-7s bel=%d on=%s seq=%d "
                  "stack=[%#x,%#x) iret.rip=%#x irsp=%#x cs=%#x canary=%s%s"
                  % (tag, i, tid, role, state_names.get(st, st), bel, on, seq,
                     sb, sb + (spg << 12), irip, irsp, ics,
                     ("ok" if canary_ok else ("BAD" if canary_ok is False else "n/a")),
                     extra))

        ok = (bad == 0 and not dup)
        print("[sc] VERDICT=%s (bad_slots=%d, double_sched=%d)"
              % ("PASS" if ok else "FAIL", bad, len(dup)))


WraithStackCheck()
