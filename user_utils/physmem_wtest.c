// SPDX-License-Identifier: GPL-2.0-only
/*
 * physmem_wtest.c — physical-memory write probe: read -> write -> re-read.
 *
 * Purpose
 *   Some firmware-reserved physical ranges (e.g. the 16 MiB at 0x100000000
 *   on this MTL laptop) are MTRR-UC and time like DRAM, but the firmware may
 *   silently drop stores ("backdoor write hole").  This tool verifies whether
 *   stores actually persist in the backing memory.
 *
 *   For each test pattern it stores the span, CLFLUSHes + MFENCEs (so a WB
 *   mapping cannot mask a dropped store via the cache), then re-reads twice
 *   and classifies:
 *     STUCK   - readback equals the pattern:  store persisted.
 *     DROPPED - readback equals the pre-test baseline: store was dropped.
 *     PARTIAL - readback differs from both: byte-level diff is printed.
 *     UNSTABLE- the two readbacks differ from each other.
 *
 *   The original content is always written back and verified before exit
 *   (also on SIGINT/SIGTERM).
 *
 * Usage:
 *   sudo ./physmem_wtest 0x100000000            # one page (4096 B)
 *   sudo ./physmem_wtest 0x100000000 256        # 256 bytes
 *   sudo ./physmem_wtest --help
 *
 * Exit status:
 *   0  all patterns persisted (writable memory)
 *   3  all patterns dropped   (write hole / read-only window)
 *   4  mixed / partial results
 *   1  error   2  usage
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAX_LEN (1u << 20) /* 1 MiB */

/* Global state for signal-time restore. */
static volatile uint8_t *g_map;
static size_t g_len;
static uint8_t *g_baseline;

static inline void fence_write(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

static inline void flush_line(const volatile void *p)
{
    __asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
}

static void flush_span(volatile uint8_t *p, size_t len)
{
    for (size_t off = 0; off < len; off += 64)
        flush_line(p + off);
    fence_write();
}

/* Write the saved baseline back and verify it stuck. */
static int restore_original(void)
{
    if (!g_map || !g_baseline || g_len == 0)
        return 0;
    memcpy((void *)g_map, g_baseline, g_len);
    fence_write();
    flush_span(g_map, g_len);
    return memcmp((const void *)g_map, g_baseline, g_len) == 0;
}

static void on_signal(int sig)
{
    restore_original();
    _exit(128 + sig);
}

static int parse_u64(const char *text, uint64_t *result)
{
    char *end;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, 0);
    if (errno || *text == '\0' || *end != '\0')
        return -1;
    *result = value;
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s [--help] PHYS_ADDR [LEN]\n"
        "\n"
        "Read->write->re-read probe on a physical range via /dev/mem.\n"
        "  PHYS_ADDR  physical address (decimal or 0x hex)\n"
        "  LEN        bytes to test (default 4096, max %u)\n"
        "\n"
        "Original bytes are restored and verified before exit.\n"
        "Exit: 0 = writable, 3 = all writes dropped, 4 = partial.\n",
        program, MAX_LEN);
}

struct pattern {
    const char *name;
    int kind;   /* 0 fill, 1 invert baseline, 2 xorshift random */
    uint8_t fill;
};

static void build_pattern(struct pattern *p, uint8_t *buf, size_t len,
                          const uint8_t *baseline)
{
    switch (p->kind) {
    case 0:
        memset(buf, p->fill, len);
        break;
    case 1:
        for (size_t i = 0; i < len; i++)
            buf[i] = (uint8_t)~baseline[i];
        break;
    case 2: {
        uint32_t x = 0x12345678u ^ (uint32_t)len;
        for (size_t i = 0; i < len; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            buf[i] = (uint8_t)x;
        }
        break;
    }
    }
}

static void print_hex(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x ", b[i]);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return 0;
    }
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    uint64_t physical;
    if (parse_u64(argv[1], &physical)) {
        fprintf(stderr, "invalid physical address: %s\n", argv[1]);
        return 2;
    }
    size_t len = 4096;
    if (argc >= 3) {
        uint64_t v;
        if (parse_u64(argv[2], &v) || v == 0 || v > MAX_LEN) {
            fprintf(stderr, "LEN must be 1..%u\n", MAX_LEN);
            return 2;
        }
        len = (size_t)v;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || ((page_size & (page_size - 1)) != 0))
        return 1;

    const uint64_t page_base = physical & ~((uint64_t)page_size - 1);
    const size_t in_page = (size_t)(physical - page_base);
    const size_t map_len = ((in_page + len + (size_t)page_size - 1) /
                            (size_t)page_size) * (size_t)page_size;

    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open /dev/mem failed: %s (run via sudo)\n", strerror(errno));
        return 1;
    }

    void *mapping = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                         fd, (off_t)page_base);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap 0x%" PRIx64 " failed: %s\n", physical, strerror(errno));
        close(fd);
        return 1;
    }
    volatile uint8_t *p = (volatile uint8_t *)mapping + in_page;

    g_map = p;
    g_len = len;
    g_baseline = malloc(len);
    if (!g_baseline) {
        fprintf(stderr, "allocation failed\n");
        munmap(mapping, map_len);
        close(fd);
        return 1;
    }
    memcpy(g_baseline, (const void *)p, len);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("physical 0x%016" PRIx64 " (mapped page 0x%016" PRIx64 "), len=%zu B\n",
           physical, page_base, len);
    printf("baseline[0..15] : ");
    print_hex(g_baseline, len < 16 ? len : 16);
    printf("\n");

    static const struct pattern patterns[] = {
        { "fill 0x00", 0, 0x00 },
        { "fill 0xFF", 0, 0xFF },
        { "fill 0xAA", 0, 0xAA },
        { "fill 0x55", 0, 0x55 },
        { "fill 0x5A", 0, 0x5A },
        { "fill 0xA5", 0, 0xA5 },
        { "fill 0x01", 0, 0x01 },
        { "fill 0xFE", 0, 0xFE },
        { "invert baseline", 1, 0 },
        { "xorshift random", 2, 0 },
    };
    const size_t npat = sizeof(patterns) / sizeof(patterns[0]);

    uint8_t *pbuf = malloc(len);
    uint8_t *rbuf = malloc(len);
    uint8_t *rbuf2 = malloc(len);
    if (!pbuf || !rbuf || !rbuf2) {
        fprintf(stderr, "allocation failed\n");
        restore_original();
        free(g_baseline);
        munmap(mapping, map_len);
        close(fd);
        return 1;
    }

    size_t n_stuck = 0, n_dropped = 0, n_partial = 0, n_unstable = 0;

    for (size_t pi = 0; pi < npat; pi++) {
        build_pattern((struct pattern *)&patterns[pi], pbuf, len, g_baseline);

        memcpy((void *)p, pbuf, len);   /* store test pattern */
        fence_write();
        flush_span(p, len);             /* defeat WB caching */
        memcpy((void *)rbuf, (const void *)p, len);
        memcpy((void *)rbuf2, (const void *)p, len);

        printf("[%-16s] write ", patterns[pi].name);
        print_hex(pbuf, len < 8 ? len : 8);
        printf("| read ");
        print_hex(rbuf, len < 8 ? len : 8);

        if (memcmp(rbuf, rbuf2, len) != 0) {
            printf("| UNSTABLE (两次重读不一致)\n");
            n_unstable++;
            n_partial++;
            continue;
        }
        if (memcmp(rbuf, pbuf, len) == 0) {
            printf("| %zu/%zu -> STUCK (写入生效)\n", len, len);
            n_stuck++;
        } else if (memcmp(rbuf, g_baseline, len) == 0) {
            printf("| %zu/%zu -> DROPPED (写入被丢弃, 读回原值)\n", len, len);
            n_dropped++;
        } else {
            size_t match = 0;
            for (size_t i = 0; i < len; i++)
                if (rbuf[i] == pbuf[i])
                    match++;
            printf("| %zu/%zu -> PARTIAL (部分生效)\n", match, len);
            printf("  diff(off, wrote, read):");
            for (size_t i = 0, shown = 0; i < len && shown < 16; i++) {
                if (rbuf[i] != pbuf[i]) {
                    printf(" [0x%zx %02x->%02x]", i, pbuf[i], rbuf[i]);
                    shown++;
                }
            }
            printf("\n");
            n_partial++;
        }
    }

    printf("\nsummary: %zu/%zu STUCK, %zu/%zu DROPPED, %zu/%zu PARTIAL%s\n",
           n_stuck, npat, n_dropped, npat, n_partial, npat,
           n_unstable ? ", UNSTABLE 出现" : "");

    int status;
    if (n_stuck == npat) {
        printf("  -> 该区间可写: 是真实内存 (UC 只是 MTRR 属性, 写入保留)\n");
        status = 0;
    } else if (n_dropped == npat) {
        printf("  -> 写入全部被丢弃: 固件\"伪内存\"/只读窗口, 不能当 RAM 用\n");
        status = 3;
    } else {
        printf("  -> 混合/部分结果: 存在地址别名、位线失效或写保护嫌疑, 建议缩小范围逐 bit 测\n");
        status = 4;
    }

    /* Restore and verify. */
    if (restore_original()) {
        printf("restore: 已还原原始 %zu B, 校验 OK\n", len);
    } else {
        printf("restore: 还原失败!! 原始内容可能已损坏, 请立即排查\n");
        status = 5;
    }

    free(pbuf);
    free(rbuf);
    free(rbuf2);
    free(g_baseline);
    munmap(mapping, map_len);
    close(fd);
    return status;
}
