// ════════════════════════════════════════════════════════════════
// printk_ring_test.cpp — printk / vprintk 宿主验收（freestanding 风味）
//
// 验收模型（按设计方 2026-09-11 定）：
//   · sink = 【一个 file】；file 内部 = mmap 缓冲区【假装是环形内存缓冲】。
//   · sink 落地路径不碰 glibc：手写字节拷贝、手写渲染；不引 STL / stdio。
//   · 各种格式 printk → 落进这个 mmap 环 → 读侧渲染出来给人看。
//
// 产物：printk_ring.bin / printk_ring.txt（格式验证）
//       printk_wrap.bin / printk_wrap.txt（回绕验证）
// ════════════════════════════════════════════════════════════════
#include "util/printk.h"

#include <cstdint>
#include <cstddef>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

// ────────── 时基：宿主单调时钟 → 微秒（extern "C" 接入点）──────────
extern "C" uint64_t now_ts_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

// ────────── lock.cpp 依赖的处理器 id（宿主恒 0）──────────
extern "C" uint32_t fast_get_processor_id() { return 0; }

// ────────── 手写内存/字符串原语（不用 glibc memcpy/strlen）──────────
static void kmemcpy(void* dst, const void* src, uint64_t n)
{
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; ++i) d[i] = s[i];
}
static uint64_t kstrlen(const char* s) { uint64_t n = 0; while (s[n]) ++n; return n; }

static void con_put(const char* s, uint64_t n) { (void)!write(1, s, n); }
static void con_str(const char* s)             { con_put(s, kstrlen(s)); }

// ────────── KFile：mmap 环（假装环形内存缓冲）──────────
// 回绕语义（本 fake 版，简化为“写满即从头开新代”）：tail + len > cap 时 tail=0、gen++。
// 真内核环的「逐条覆盖」留到移植时；此处保证读侧看到的是自洽的一段线性记录流。
struct KFile
{
    uint8_t*        base;
    uint64_t        cap;
    uint64_t        tail;     // 下一个写入偏移
    uint32_t        seq;      // 记录序号
    uint32_t        gen;      // 代（回绕次数）
    uint64_t        dropped;  // 因回绕被丢弃的“代”
    spinlock_cpp_t  lock;
};

static void kfile_write(KFile* f, const char* buf, uint64_t len)
{
    if (len > f->cap) { ++f->dropped; return; }
    if (f->tail + len > f->cap) { f->tail = 0; ++f->gen; ++f->dropped; }  // 回绕：开新代
    kmemcpy(f->base + f->tail, buf, len);
    f->tail += len;
}

// ────────── ring sink：render_prefix 写二进制头；emit 落 file ──────────
static uint32_t ring_render_prefix(void* self, char* buf, uint64_t cap,
                                   klog::level_t level, uint64_t ts_us)
{
    if (cap < sizeof(klog::log_rec_hdr)) return 0;
    KFile* f = (KFile*)self;
    klog::log_rec_hdr* h = (klog::log_rec_hdr*)buf;
    h->len   = 0;                       // 占位，emit 回填
    h->_rsv  = 0;
    h->level = (uint8_t)level;
    h->seq   = f->seq++;
    h->ts_us = ts_us;
    return (uint32_t)sizeof(klog::log_rec_hdr);
}

static void ring_emit(void* self, char* buf, uint64_t len)
{
    KFile* f = (KFile*)self;
    klog::log_rec_hdr* h = (klog::log_rec_hdr*)buf;
    h->len = (uint16_t)(len - sizeof(klog::log_rec_hdr));   // 回填 body 长度
    kfile_write(f, buf, len);
}

// ────────── 读侧渲染（手写，不用 printf）──────────
static const char* lvl_name(uint8_t l)
{
    switch (l)
    {
        case klog::level::INFO:    return "INFO";
        case klog::level::NOTICE:  return "NOTICE";
        case klog::level::WARNING: return "WARN";
        case klog::level::ERROR:   return "ERROR";
        case klog::level::FATAL:   return "FATAL";
        default:                   return "?";
    }
}

// 把 u64 写成十进制（返回写入 out 的字节数）
static uint64_t u64_dec(char* out, uint64_t v)
{
    char t[24]; uint64_t n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = char('0' + v % 10); v /= 10; }
    uint64_t i = 0;
    for (uint64_t k = n; k > 0; --k) out[i++] = t[k - 1];
    return i;
}

// 渲染一条记录为文本行：[seq] sec.usec LEVEL body\n
static uint64_t render_record(char* out, const KFile* f, uint64_t off)
{
    const klog::log_rec_hdr* h = (const klog::log_rec_hdr*)(f->base + off);
    const char* body = (const char*)(f->base + off + sizeof(klog::log_rec_hdr));
    uint64_t n = 0;
    out[n++] = '['; n += u64_dec(out + n, h->seq); out[n++] = ']'; out[n++] = ' ';
    uint64_t sec = h->ts_us / 1000000ull, us = h->ts_us % 1000000ull;
    n += u64_dec(out + n, sec);
    out[n++] = '.';
    { char u[6]; for (int i = 5; i >= 0; --i) { u[i] = char('0' + us % 10); us /= 10; }
      for (int i = 0; i < 6; ++i) out[n++] = u[i]; }
    out[n++] = ' ';
    const char* nm = lvl_name(h->level);
    while (*nm) out[n++] = *nm++;
    out[n++] = ' ';
    for (uint16_t i = 0; i < h->len; ++i) out[n++] = body[i];
    out[n++] = '\n';
    return n;
}

// 遍历环 [0,tail) 并渲染到 con + 可选落文件
static void dump_ring(const char* title, const KFile* f, const char* txt_path)
{
    con_str("\n========== "); con_str(title); con_str(" ==========\n");
    char line[klog::LOG_LINE_MAX + 64];
    uint64_t off = 0;
    int fd = open(txt_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    while (off + sizeof(klog::log_rec_hdr) <= f->tail)
    {
        const klog::log_rec_hdr* h = (const klog::log_rec_hdr*)(f->base + off);
        uint64_t rec = sizeof(klog::log_rec_hdr) + h->len;
        if (off + rec > f->tail) break;
        uint64_t ln = render_record(line, f, off);
        con_put(line, ln);
        if (fd >= 0) (void)!write(fd, line, ln);
        off += rec;
    }
    if (fd >= 0) close(fd);
    con_str("--- gen="); { char b[16]; uint64_t l=u64_dec(b,f->gen); con_put(b,l);} 
    con_str(" seq=");    { char b[16]; uint64_t l=u64_dec(b,f->seq); con_put(b,l);} 
    con_str(" dropped_gens="); { char b[16]; uint64_t l=u64_dec(b,f->dropped); con_put(b,l);} 
    con_str(" bytes_used=");   { char b[16]; uint64_t l=u64_dec(b,f->tail); con_put(b,l);} 
    con_str("/");              { char b[16]; uint64_t l=u64_dec(b,f->cap); con_put(b,l);} 
    con_str("\n");
}

// ────────── mmap 建 file ──────────
static KFile* kfile_open(const char* path, uint64_t cap)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return nullptr;
    if (ftruncate(fd, (off_t)cap) != 0) { close(fd); return nullptr; }
    void* m = mmap(nullptr, cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return nullptr;
    KFile* f = new KFile();
    f->base = (uint8_t*)m; f->cap = cap; f->tail = 0; f->seq = 0; f->gen = 0; f->dropped = 0;
    return f;
}

// ────────── 各种格式 battery ──────────
static void format_battery()
{
    klog::printk(klog::level::INFO, "== printk format battery ==");
    klog::printk(klog::level::INFO, "dec: %d %i %u %lld %llu", -42, 42, 42u, -9000000000ll, 9000000000ull);
    klog::printk(klog::level::INFO, "hex: %x %X %#x %#X %08x", 0xdeadbeefu, 0xdeadbeefu, 0xbeefu, 0xbeefu, 0x1234u);
    klog::printk(klog::level::INFO, "oct/bin: %o %#o %b %#b", 0755u, 0755u, 0b1011u, 0b1011u);
    klog::printk(klog::level::INFO, "width/prec: [%5d] [%-5d] [%05d] [%.3d] [%8.3d]", 42, 42, 42, 7, 7);
    klog::printk(klog::level::INFO, "sign: [%+d] [% d] [%+d] [% d]", 5, 5, -5, -5);
    klog::printk(klog::level::INFO, "str: [%s] [%10s] [%-10s] [%.3s]", "hello", "hi", "hi", "hello");
    klog::printk(klog::level::INFO, "char: [%c] [%5c] [%-5c]", 'A', 'B', 'C');
    klog::printk(klog::level::INFO, "ptr: %p %p", (void*)0x0, (void*)0xdeadbeefcafef00dULL);
    klog::printk(klog::level::INFO, "star: [%*d] [%-*d] [%.*d]", 6, 42, 6, 42, 5, 7);
    klog::printk(klog::level::INFO, "pct: 100%% done");
    klog::printk(klog::level::NOTICE, "notice level sample %d", 1);
    klog::printk(klog::level::WARNING, "warning level sample %s", "warn");
    klog::printk(klog::level::ERROR, "error level sample 0x%x", 0xbad);
    klog::printk(klog::level::FATAL, "fatal level sample %llu", 0xffffffffffffffffull);
    klog::printk(klog::level::INFO, "long line test: %s", "abcdefghij0123456789abcdefghij0123456789abcdefghij0123456789");
    klog::early_printk("early_printk sample %d %s", 7, "tag");
    // 截断验证：超长串撞满 LOG_LINE_MAX
    klog::printk(klog::level::ERROR, "trunc: %s", 
        "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEEFFFFFFFFFFGGGGGGGGGGHHHHHHHHHHIIIIIIIIIIJJJJJJJJJJ"
        "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEEFFFFFFFFFFGGGGGGGGGGHHHHHHHHHHIIIIIIIIIIJJJJJJJJJJ"
        "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEEFFFFFFFFFFGGGGGGGGGGHHHHHHHHHHIIIIIIIIIIJJJJJJJJJJ"
        "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEEFFFFFFFFFFGGGGGGGGGGHHHHHHHHHHIIIIIIIIIIJJJJJJJJJJ"
        "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEEFFFFFFFFFFGGGGGGGGGGHHHHHHHHHHIIIIIIIIIIJJJJJJJJJJ"
        "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEEFFFFFFFFFFGGGGGGGGGGHHHHHHHHHHIIIIIIIIIIJJJJJJJJJJ"
        "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEEFFFFFFFFFFGGGGGGGGGGHHHHHHHHHHIIIIIIIIIIJJJJJJJJJJ"
        "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEEFFFFFFFFFFGGGGGGGGGGHHHHHHHHHHIIIIIIIIIIJJJJJJJJJJ"
        "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEEFFFFFFFFFFGGGGGGGGGGHHHHHHHHHHIIIIIIIIIIJJJJJJJJJJ"
        "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEEFFFFFFFFFFGGGGGGGGGGHHHHHHHHHHIIIIIIIIIIJJJJJJJJJJ"
        "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEEFFFFFFFFFFGGGGGGGGGGHHHHHHHHHHIIIIIIIIIIJJJJJJJJJJ"
        "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEEFFFFFFFFFFGGGGGGGGGGHHHHHHHHHHIIIIIIIIIIJJJJJJJJJJ");
}

static void wrap_demo()
{
    // 小环（512B）：写满即回绕。用来验证 gen/dropped + 只留最新代。
    KFile* f = kfile_open("printk_wrap.bin", 512);
    if (!f) return;
    klog::log_sink sink{ ring_render_prefix, ring_emit, f, &f->lock };
    klog::set_default_sink(&sink);
    for (uint32_t i = 0; i < 40; ++i)
        klog::printk(klog::level::INFO, "wrap line #%u payload=%08x", i, i * 0x01010101u);
    dump_ring("WRAP RING (512B, gen/dropped 演示)", f, "printk_wrap.txt");
}

int main()
{
    KFile* f = kfile_open("printk_ring.bin", 64 * 1024);   // 64KiB：格式验证不触发回绕
    if (!f) { con_str("open/mmap failed\n"); return 1; }

    klog::log_sink ring_sink{ ring_render_prefix, ring_emit, f, &f->lock };
    klog::set_default_sink(&ring_sink);                    // runtime 绑死这个 file

    format_battery();
    dump_ring("PRINTK RING (64KiB)", f, "printk_ring.txt");

    wrap_demo();
    return 0;
}
