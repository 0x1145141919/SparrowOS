// ════════════════════════════════════════════════════════════════
// init_printk.cpp — kernel.elf 启动期日志面层（入口 → create_first_kthread）
//
// 与 init.elf 的 init_printk 同签名；三后端广播：环 / UART / GOP；无 "[INIT] " 前缀。
//
// 环 = 继承自 init.elf 的同一条 v2 环（DmesgRingBuffer_v2，转生凭证见 abi/kring_soul.h）：
//   bringup 时经主窗口把物理凭证重链成内核 VA，重绑后 odometer / record_count 连续，
//   init 跳转前写的记录与 kernel 的第一条无缝接续 —— 这是「从开始打 log 就不中断」。
//
// 时基：now_ts_us() 接 ktime（TCG→HPET / KVM·BARE→TSC）。未就绪（MM 前 / KVM tsc 未校）
//   返回 0 → 记录头 ts=0、文本前缀省略时间戳段（与 init 早期同构）。
//
// 并发：调用窗口 = create_first_kthread 之前（单 BSP，无抢占），故不取锁。
// ════════════════════════════════════════════════════════════════
#include "util/init_printk.h"
#include "kcirclebufflogMgr.h"           // DmesgRingBuffer_v2 / log_record_head_compressed
#include "abi/kring_soul.h"              // DmesgRing_handoff
#include "abi/asset_names.h"             // asset_names::ring_log
#include "abi/os_error_definitions.h"    // level_code
#include "boot/asset_table.h"            // g_asset_table
#include "memory/main_phyaddr_access_window.h"  // PHYACC_VA
#include "arch/x86_64/core_hardwares/PortDriver.h"  // polling_puts
#include "util/textConsole.h"            // textconsole_GoP
#include "ktime.h"                       // ktime::get_microsecond_stamp

// —— 时基符号（extern "C"，kcirclebufflogMgr / printk 均声明此符号）——
extern "C" uint64_t now_ts_us() { return ktime::get_microsecond_stamp(); }

namespace {

// kernel 侧持有的继承环句柄（bringup 时置；就绪前 nullptr）
DmesgRingBuffer_v2* g_ring   = nullptr;
bool                g_online = false;

// 文本后端前缀 "[<ts_us>] [<seq>] "（body 无 [INIT]）—— 手写 u64 十进制，免依赖。
// 与 init 侧 build_text_prefix 同款，唯一定义处。
uint32_t build_text_prefix(char* out, uint32_t cap, uint64_t ts_us, uint64_t seq)
{
    uint32_t n = 0;
    auto put  = [&](char c) { if (n < cap) out[n] = c; ++n; };
    auto putu = [&](uint64_t v) {
        char t[24]; int k = 0;
        if (v == 0) t[k++] = '0';
        while (v) { t[k++] = char('0' + (int)(v % 10)); v /= 10; }
        for (int i = k - 1; i >= 0; --i) put(t[i]);
    };
    put('['); putu(ts_us); put(']'); put(' ');
    put('['); putu(seq);   put(']'); put(' ');
    return (n < cap) ? n : cap;
}

}  // anonymous namespace

// ─────────────────────────── bringup ───────────────────────────
void init_printk_bringup()
{
    if (g_online) return;

    // 认领继承的 ring_log blob（纯物理凭证，不做内容拷贝）
    if (g_asset_table) {
        const asset_table_entry* e = g_asset_table->read(asset_names::ring_log);   // arg0=ring_log
        if (e) {
            const DmesgRing_handoff h = *(const DmesgRing_handoff*)e->data;
            g_asset_table->deal(asset_names::ring_log);   // full_name 全匹配

            // init 的活体环对象（.bss，kernel_persisit 穿越）→ 经主窗口访问
            DmesgRingBuffer_v2* obj   = (DmesgRingBuffer_v2*)PHYACC_VA(h.obj_pbase);
            DmesgRingBuffer_soul soul = *obj->get_soul();          // 沿用 odometer / record_count
            soul.buff = (void*)PHYACC_VA(h.buff_pbase);            // 缓冲重绑到本窗口 VA
            obj->Reincarnate(&soul);
            g_ring = obj;
        }
    }
    g_online = true;
}

// ─────────────────────────── 面层 ───────────────────────────
void init_printk(const char* fmt, ...)
{
    if (!g_online) return;

    char     buf[klog::LOG_LINE_MAX];
    va_list  ap;
    va_start(ap, fmt);
    uint64_t res = klog::vprintkv2(fmt, ap, buf, (uint32_t)sizeof(buf));
    va_end(ap);
    uint32_t n = (uint32_t)(res & 0xFFFFFFFFu);
    if (n == 0) return;

    // 前缀复用环回传的同一 ts / seq（与记录头一致）
    uint64_t ts  = 0;
    uint64_t seq = 0;
    if (g_ring) {
        log_record_head_compressed h =
            g_ring->record_add((uint16_t)n, (uint8_t)level_code::INFO);
        g_ring->putsk(buf, (uint16_t)n);
        ts  = h.ts_us;
        seq = h.record_seq;
    }

    char     pfx[64];
    uint32_t pl = build_text_prefix(pfx, sizeof(pfx), ts, seq);

    // UART(COM1)
    polling_puts(pfx, (uint64_t)pl);
    polling_puts(buf, (uint64_t)n);
    polling_puts("\r\n", 2);

    // GOP 文本控制台
    textconsole_GoP::PutString(pfx, (uint64_t)pl);
    textconsole_GoP::PutString(buf, (uint64_t)n);
    textconsole_GoP::PutChar('\n');
}
