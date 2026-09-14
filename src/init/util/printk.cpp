// ════════════════════════════════════════════════════════════════
// printk.cpp（init.elf 专用面层）
//
// 流程：固定 "[INIT] " 前缀（调用点不写，全由函数加）→ vprintkv2 写 body
// → 整条落 ring_log（record_add 截获 {ts_us,seq} → putsk）
// → 文本后端按【编译期宏】决定是否真调打印函数（GOP / UART）。
//
// 无 level（固定 INFO）/ 无锁（单线程）/ ts 来自环境提供的 now_ts_us()。
// ════════════════════════════════════════════════════════════════
#include "init/util/printk.h"
#include "init/util/ring_log.h"
#include "abi/os_error_definitions.h"   // level_code

// —— 文本后端编译期开关（至少一套宏控制）——
#ifndef INIT_LOG_UART
#  define INIT_LOG_UART 1
#endif
#ifndef INIT_LOG_GOP
#  define INIT_LOG_GOP 0
#endif

#if INIT_LOG_UART
#  include "init/core_hardwares/PortDriver.h"   // uart_write()
#endif
#if INIT_LOG_GOP
#  include "init/util/textConsole.h"            // init_textconsole::PutString / PutChar
#endif

static const char INIT_PREFIX[] = "[INIT] ";    // 初始化阶段固定标记，函数内部统一加

// 文本后端前缀 "[<ts_us>] [<seq>] "（body 自带 [INIT]）—— 手写 u64 十进制，免依赖
static uint32_t build_text_prefix(char* out, uint32_t cap, uint64_t ts_us, uint64_t seq)
{
    uint32_t n = 0;
    auto put   = [&](char c) { if (n < cap) out[n] = c; ++n; };
    auto putu  = [&](uint64_t v) {
        char t[24]; int k = 0;
        if (v == 0) t[k++] = '0';
        while (v) { t[k++] = char('0' + (int)(v % 10)); v /= 10; }
        for (int i = k - 1; i >= 0; --i) put(t[i]);
    };
    put('['); putu(ts_us); put(']'); put(' ');
    put('['); putu(seq);   put(']'); put(' ');
    return (n < cap) ? n : cap;
}

void init_printk(const char* fmt, ...)
{
    char buf[klog::LOG_LINE_MAX];

    // 1) 固定前缀（无 level、无 ts）
    uint64_t p = 0;
    while (INIT_PREFIX[p]) { buf[p] = INIT_PREFIX[p]; ++p; }

    // 2) body：唯一 formatter（freestanding 核心）
    va_list ap;
    va_start(ap, fmt);
    uint32_t room = (uint32_t)sizeof(buf) - (uint32_t)p;
    uint64_t res  = klog::vprintkv2(fmt, ap, buf + p, room);
    va_end(ap);

    uint16_t n     = (uint16_t)(res & 0xFFFFFFFFu);   // 实际写入字节数
    uint16_t total = (uint16_t)(p + n);               // [INIT] 前缀计入记录长度

    // 3) 落环：record_add 插头回传本记录 {ts_us, seq}，随后 putsk 落 body（环未绑则空转）
    log_record_head_compressed hdr = ring_log.record_add(total, (uint8_t)level_code::INFO);
    ring_log.putsk(buf, total);

    // 4) 文本后端（宏控制）：前缀复用环回传的同一 ts/seq，body 为 buf（含 [INIT]）
#if INIT_LOG_UART || INIT_LOG_GOP
    char     pfx[64];
    uint32_t pl = build_text_prefix(pfx, sizeof(pfx), hdr.ts_us, hdr.record_seq);
#endif
#if INIT_LOG_UART
    uart_write(pfx, (uint64_t)pl);
    uart_write(buf, (uint64_t)total);
    uart_write("\r\n", 2);
#endif
#if INIT_LOG_GOP
    init_textconsole::PutString(pfx, pl);
    init_textconsole::PutString(buf, total);
    init_textconsole::PutChar('\n');
#endif
}
