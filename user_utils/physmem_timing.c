// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only /dev/mem timing probe for comparing physical-memory mappings.
 *
 * This program never requests PROT_WRITE and never stores through a mapping.
 * It maps exactly one page for each supplied physical address.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

enum { DEFAULT_SAMPLES = 20000, MAX_SAMPLES = 1000000, STREAM_LOADS = 1000000 };

static inline uint64_t tsc_begin(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t tsc_end(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtscp\n\tlfence" : "=a"(lo), "=d"(hi) :: "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline void evict_line(const volatile void *address)
{
    __asm__ volatile("clflush (%0)\n\tmfence" :: "r"(address) : "memory");
}

static int compare_u64(const void *a, const void *b)
{
    const uint64_t aa = *(const uint64_t *)a;
    const uint64_t bb = *(const uint64_t *)b;
    return (aa > bb) - (aa < bb);
}

static uint64_t percentile(const uint64_t *samples, size_t count, unsigned pct)
{
    return samples[((count - 1) * pct) / 100];
}

static int parse_u64(const char *text, uint64_t *result)
{
    char *end;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, 0);
    if (errno || *text == '\0' || *end != '\0') return -1;
    *result = value;
    return 0;
}

static int pin_cpu(unsigned cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

static void usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s [--samples N] [--cpu N] PHYS_ADDR [PHYS_ADDR ...]\n"
        "\n"
        "Maps one read-only page per address through /dev/mem and reports:\n"
        "  cold: load after CLFLUSH (serialized cycles);\n"
        "  hot:  second load of that same line (serialized cycles);\n"
        "  stream: repeated loads from one address (cycles/load).\n"
        "\n"
        "Addresses accept decimal or 0x hexadecimal notation. No writes are issued.\n",
        program);
}

static int measure_one(int fd, uint64_t physical, size_t page_size, size_t count)
{
    const uint64_t page_base = physical & ~((uint64_t)page_size - 1);
    const size_t in_page = (size_t)(physical - page_base);
    void *mapping = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd, (off_t)page_base);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "0x%" PRIx64 ": mmap failed: %s\n", physical, strerror(errno));
        return -1;
    }

    volatile uint8_t *address = (volatile uint8_t *)mapping + in_page;
    uint64_t *cold = calloc(count, sizeof(*cold));
    uint64_t *hot = calloc(count, sizeof(*hot));
    if (!cold || !hot) {
        fprintf(stderr, "allocation failed\n");
        free(cold); free(hot); munmap(mapping, page_size);
        return -1;
    }

    /* Fault the page before recording samples. */
    volatile uint8_t sink = *address;
    (void)sink;
    for (size_t i = 0; i < count; ++i) {
        volatile uint8_t *line = address + ((i * 64u) & (page_size - 64u));
        evict_line(line);
        uint64_t before = tsc_begin();
        sink ^= *line;
        cold[i] = tsc_end() - before;
        before = tsc_begin();
        sink ^= *line;
        hot[i] = tsc_end() - before;
    }

    uint64_t stream_before = tsc_begin();
    for (size_t i = 0; i < STREAM_LOADS; ++i)
        sink ^= *address;
    uint64_t stream_cycles = tsc_end() - stream_before;

    qsort(cold, count, sizeof(*cold), compare_u64);
    qsort(hot, count, sizeof(*hot), compare_u64);
    printf("physical 0x%016" PRIx64 " (mapped page 0x%016" PRIx64 ")\n", physical, page_base);
    printf("  cold cycles: min=%" PRIu64 " p01=%" PRIu64 " p50=%" PRIu64 " p99=%" PRIu64 "\n",
           cold[0], percentile(cold, count, 1), percentile(cold, count, 50), percentile(cold, count, 99));
    printf("  hot  cycles: min=%" PRIu64 " p01=%" PRIu64 " p50=%" PRIu64 " p99=%" PRIu64 "\n",
           hot[0], percentile(hot, count, 1), percentile(hot, count, 50), percentile(hot, count, 99));
    printf("  stream: %.2f cycles/load (%d repeated read-only loads)\n",
           (double)stream_cycles / STREAM_LOADS, STREAM_LOADS);
    free(cold);
    free(hot);
    munmap(mapping, page_size);
    return 0;
}

int main(int argc, char **argv)
{
    size_t samples = DEFAULT_SAMPLES;
    int first_address = 1;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || ((page_size & (page_size - 1)) != 0)) return 1;

    while (first_address < argc && argv[first_address][0] == '-') {
        if (!strcmp(argv[first_address], "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(argv[first_address], "--samples") && first_address + 1 < argc) {
            uint64_t value;
            if (parse_u64(argv[first_address + 1], &value) || value == 0 || value > MAX_SAMPLES) {
                fprintf(stderr, "--samples must be 1..%d\n", MAX_SAMPLES); return 2;
            }
            samples = (size_t)value; first_address += 2; continue;
        }
        if (!strcmp(argv[first_address], "--cpu") && first_address + 1 < argc) {
            uint64_t value;
            if (parse_u64(argv[first_address + 1], &value) || value >= CPU_SETSIZE || pin_cpu((unsigned)value)) {
                fprintf(stderr, "cannot pin to requested CPU: %s\n", strerror(errno)); return 2;
            }
            first_address += 2; continue;
        }
        usage(argv[0]); return 2;
    }
    if (argc - first_address < 1) { usage(argv[0]); return 2; }

    int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open /dev/mem failed: %s (normally run this binary via sudo)\n", strerror(errno));
        return 1;
    }
    int status = 0;
    for (int i = first_address; i < argc; ++i) {
        uint64_t physical;
        if (parse_u64(argv[i], &physical)) {
            fprintf(stderr, "invalid physical address: %s\n", argv[i]); status = 2; continue;
        }
        if (measure_one(fd, physical, (size_t)page_size, samples)) status = 1;
    }
    close(fd);
    return status;
}
