#include "init/phase_3.h"
#include "abi/asset_names.h"
#include "init/page_allocator_v2.h"
#include "init/init_fatal.h"
#include "init/initramfs_lookup.h"
#include "init/util/printk.h"
#include <elf.h>

// ============================================================================
// Phase 3a (串行): kernel.elf 解包 → 精确狙击 4 段进 kmmu → 产出进资产容器
// ============================================================================
//
// 本函数是纯"产出方"：产物全部进资产容器（隐式状态）。
// 失败语义统一为 fatal：任一环节出错（无 initramfs / ELF 损坏 / OOM / 段缺失 /
// kmmu 失败 / 资产重复）都直接 init_fatal::halt(SRC_LOC())，仅成功路径返回 0。
//   - 四段 kernel_code/data/rodata/bss → "kernel_* mem"（vm_interval desc）
//   - kIMG（kernel.elf 瞬态文件映像）→ "kimg movable"（movable_file_entry_t desc）
//   - 入口点 → 经 entry_vaddr_out 直出（init 内部消费，不进 handoff 资产注册表）
// ctx/header 管线退居幕后（偶然复杂度），调用点从容器按 arg0 取资产。
//
// 设计：
//   1. kernel.elf 完整文件 → 瞬态端分配 kimg_pbase（kIMG 文件的家，经恒等窗口访问）
//   2. 不再遍历所有 PT_LOAD 加载；按段名精确命中 4 个段：
//        .text(.text_main) / .data / .rodata / .bss
//      每段三关：名字匹配 → 确认在 PT_LOAD 内 → is_kernel_address 确认内核区间
//   3. 独立分配物理页 + 拷贝/清零 + 进入 kmmu（kernel_code/data/rodata/bss 名字）
//   4. 所属 LOAD 的 p_paddr 写回真实 PA（kernel.elf 经 kIMG 自省契约不变）
//   5. kIMG 不做专用映射（恒等窗口已覆盖），登记 movable 资产即可
//   ap_bootstrap 低地址段不做加载（本函数只管内核区间资产）
//
// 依赖：kld.ld 下每个主段独立成 LOAD（section ⊇ LOAD 1:1），p_paddr 写回即整段真实 PA。
//
loc_code_t phase_3a_load_kernel(kernel_mmu* kmmu, const ctx_early_mem* em,
                                BootInfoHeader* /*header*/, uint64_t* entry_vaddr_out) {
    phyaddr_t kimg_pbase = 0;

    // ---- 1. 从 initramfs 定位 kernel.elf，拷贝到瞬态端 ----
    if (em->ramfs_base == 0) {
        init_printk("Phase 3a: initramfs not present"); init_fatal::halt(SRC_LOC());
    }
    const initramfs_header* rh = (const initramfs_header*)(uint64_t)em->ramfs_base;
    uint64_t kelf_sz = 0;
    phyaddr_t kelf_in_ramfs = initramfs_lookup(rh, "/kernel.elf", &kelf_sz);
    if (kelf_in_ramfs == 0 || kelf_sz == 0) {
        init_printk("Phase 3a: initramfs_lookup failed"); init_fatal::halt(SRC_LOC());
    }
    uint64_t kelf_pages = align_up(kelf_sz, 4096) >> 12;
    kimg_pbase = page_allocator_v2::free_ram_explore(kelf_pages, 12);
    if (kimg_pbase == 0) {
        init_printk("Phase 3a: transient OOM: %lx pages", (unsigned long)kelf_pages); init_fatal::halt(SRC_LOC());
    }
    // kimg = 从 initramfs 解包出的 kernel.elf 瞬态文件 → kernel_file_property
    if (page_allocator_v2::pages_set({kimg_pbase, kelf_pages << 12},
                                     page_state_t::kernel_file_property) != 0) {
        init_printk("Phase 3a: kimg pages_set failed"); init_fatal::halt(SRC_LOC());
    }
    ksystemramcpy((void*)(uint64_t)kelf_in_ramfs, (void*)(uint64_t)kimg_pbase, kelf_sz);
    ksetmem_8((void*)(uint64_t)(kimg_pbase + kelf_sz), 0, (kelf_pages << 12) - kelf_sz);
    init_printk("Phase 3a: kernel.elf (transient) at 0x%lx size=%lx",
                (unsigned long)kimg_pbase, (unsigned long)kelf_sz);

    // ---- 2. ELF header 校验 ----
    uint8_t* elf_base = (uint8_t*)(uint64_t)kimg_pbase;
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)elf_base;
    if (ehdr->e_ident[EI_MAG0]!=ELFMAG0||ehdr->e_ident[EI_MAG1]!=ELFMAG1||
        ehdr->e_ident[EI_MAG2]!=ELFMAG2||ehdr->e_ident[EI_MAG3]!=ELFMAG3) {
        init_printk("Phase 3a: bad magic"); init_fatal::halt(SRC_LOC());
    }
    init_printk("Phase 3a: phnum=%x shnum=%x entry=0x%lx",
                (unsigned)ehdr->e_phnum, (unsigned)ehdr->e_shnum, (unsigned long)ehdr->e_entry);

    // 入口点经 out-param 直出（仅 init 侧 phase_4.5 跳转消费，kernel.elf 无此消费方，
    // 不进 handoff 资产注册表）
    if (entry_vaddr_out) *entry_vaddr_out = ehdr->e_entry;

    // ---- 3. 段表解析 + 精确狙击 4 段 ----
    if (ehdr->e_shnum == 0 || ehdr->e_shstrndx == SHN_UNDEF) {
        init_printk("Phase 3a: no section headers"); init_fatal::halt(SRC_LOC());
    }
    Elf64_Shdr* shdr = (Elf64_Shdr*)(elf_base + ehdr->e_shoff);
    Elf64_Shdr& shstr_hdr = shdr[ehdr->e_shstrndx];
    const char* shstrtab = (const char*)(elf_base + shstr_hdr.sh_offset);

    struct sec_target_t {
        const char* name_key;    // 段名匹配键
        bool        prefix;      // true=前缀匹配（.text 兼容 .text_main）
        const char* kmmu_name;   // 进 kmmu 的名字（arg0 语义）
        const char* asset_name;  // 进资产树的多arg 名字（arg0 + arg1 路由）
    };
    const sec_target_t k_sec_targets[] = {
        { ".text",   true,  "kernel_code",   asset_names::kernel_code   },
        { ".data",   false, "kernel_data",   asset_names::kernel_data   },
        { ".rodata", false, "kernel_rodata", asset_names::kernel_rodata },
        { ".bss",    false, "kernel_bss",    asset_names::kernel_bss    },
    };
    constexpr int k_sec_count = 4;

    // 名字匹配：prefix ? 前缀 : 精确
    auto sec_name_hit = [](const char* sname, const sec_target_t& tg) -> bool {
        if (tg.prefix) {
            return strncmp_in_kernel(sname, tg.name_key, strlen_in_kernel(tg.name_key)) == 0;
        }
        return strcmp_in_kernel(sname, tg.name_key) == 0;
    };

    uint64_t kernel_vaddr_top = 0;

    // 加载单个命中段：PT_LOAD 确认 → 内核区间确认 → 分配/拷贝/清零 → kmmu → 资产树
    // 失败即 halt（无降级路径）：打印出错点 + 位置戳后停机，仅成功路径正常返回。
    auto load_section = [&](Elf64_Shdr& sh, const char* kmmu_name, const char* asset_name) -> void {
        auto fail_halt = [&](const char* why) {
            init_printk("Phase 3a: %s: %s", kmmu_name, why);
            init_fatal::halt(SRC_LOC());
        };
        if (sh.sh_size == 0) fail_halt("section size 0");

        // ---- a. 确认属于某个 PT_LOAD ----
        Elf64_Phdr* owner = nullptr;
        for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
            Elf64_Phdr* ph = (Elf64_Phdr*)(elf_base + ehdr->e_phoff + i * ehdr->e_phentsize);
            if (ph->p_type != PT_LOAD) continue;
            if (sh.sh_addr >= ph->p_vaddr &&
                sh.sh_addr + sh.sh_size <= ph->p_vaddr + ph->p_memsz) {
                owner = ph;
                break;
            }
        }
        if (!owner) fail_halt("not in any PT_LOAD");

        // ---- b. 内核区间确认（is_kernel_address） ----
        uint64_t sz  = align_up(sh.sh_size, 4096);
        uint64_t npg = sz >> 12;
        pgaccess acc;
        acc.is_kernel     = 1;
        acc.is_writeable  = (sh.sh_flags & SHF_WRITE) ? 1 : 0;
        acc.is_readable   = 1;
        acc.is_executable = (sh.sh_flags & SHF_EXECINSTR) ? 1 : 0;
        acc.is_global     = 1;
        acc.cache_strategy = WB;
        vm_interval iv = {.vpn = sh.sh_addr >> 12, .ppn = 0, .npages = npg, .access = acc};
        if (!iv.is_kernel_address()) {
            init_printk("Phase 3a: %s not kernel range 0x%lx", kmmu_name, (unsigned long)sh.sh_addr);
            init_fatal::halt(SRC_LOC());
        }

        // ---- c. 独立分配物理页 + 内容安置 ----
        if (sh.sh_type != SHT_NOBITS) {
            if (sh.sh_offset + sh.sh_size > kelf_pages * 4096ULL)
                fail_halt("offset out of kIMG range");
        }
        // 物理分配对齐上调：取 min(段虚拟基址自然对齐, 段大小上界 2 的幂)，封顶 1GB。
        // 例：.text_main sh_addr=0xFFFF800000000000 基址 2MB 对齐 → 分配也 2MB 对齐，
        //     使 kmmu 映射能上 2MB/1GB 大页（kld.ld 中主段起始均按 ALIGN(2M) 布局）。
        int align_log2 = 12;
        while (align_log2 < 30 && (sh.sh_addr & (1ULL << align_log2)) == 0) {
            align_log2++;   // sh_addr 最低置位位 → 自然基址对齐
        }
        int sz_log = 12;
        while (sz_log < 30 && (sz >> sz_log) > 1) {
            sz_log++;       // floor(log2(sz))：页面粒度上界
        }
        if (sz_log < align_log2) align_log2 = sz_log;
        phyaddr_t pa = page_allocator_v2::free_ram_explore(npg, align_log2);
        if (pa == 0) fail_halt("alloc OOM");
        // kernel.elf 四大核心段 = 内核持久元数据 → kernel_persisit
        if (page_allocator_v2::pages_set({pa, npg << 12},
                                         page_state_t::kernel_persisit) != 0)
            fail_halt("pages_set failed");
        if (sh.sh_type == SHT_NOBITS) {
            ksetmem_8((void*)(uint64_t)pa, 0, sz);   // .bss：清零
        } else {
            ksystemramcpy((void*)(uint64_t)(kimg_pbase + sh.sh_offset),
                          (void*)(uint64_t)pa, sh.sh_size);
            if (sz > sh.sh_size) {
                ksetmem_8((void*)(uint64_t)(pa + sh.sh_size), 0, sz - sh.sh_size);
            }
        }

        // 写回真实 PA 到所属 LOAD 的 p_paddr（kld.ld 下段⊇LOAD 1:1，整段即真实 PA）
        owner->p_paddr = pa;

        // ---- d. 进入 kmmu（名字台账，PERSISTENT 交 kernel 认领） ----
        if (kmmu->map(kernel_mmu::make_entry(pa, sh.sh_addr, sz, acc,
                                             kmmu_name, KMMU_ENTRY_FLAG_PERSISTENT)) != 0)
            fail_halt("kmmu map fail");

        // ---- e. 登记资产树（handoff 清单）：与 kmmu 台账同 arg0 的持久 desc ----
        //      desc = vm_interval（init 堆分配，随条目生命周期持久），路由 arg1 = "mem"。
        vm_interval* adesc = new vm_interval{ .vpn   = sh.sh_addr >> 12,
                                              .ppn   = pa >> 12,
                                              .npages = npg,
                                              .access = acc };
        if (!asset_reg_add(asset_name, adesc)) fail_halt("asset dup");

        uint64_t vend = sh.sh_addr + sh.sh_size;
        if (vend > kernel_vaddr_top) kernel_vaddr_top = vend;
        init_printk("Phase 3a: %s: v=0x%lx p=0x%lx sz=0x%lx",
                    kmmu_name, (unsigned long)sh.sh_addr, (unsigned long)pa, (unsigned long)sz);
    };

    // 对四个目标段分别命中（每个命中首个匹配段）并加载
    for (int t = 0; t < k_sec_count; t++) {
        const sec_target_t& tg = k_sec_targets[t];
        bool matched = false;
        for (Elf64_Half si = 0; si < ehdr->e_shnum; si++) {
            if (shdr[si].sh_type == SHT_NULL) continue;
            const char* sname = shstrtab + shdr[si].sh_name;
            if (!sec_name_hit(sname, tg)) continue;
            matched = true;
            load_section(shdr[si], tg.kmmu_name, tg.asset_name);   // 失败内部 halt
            break;
        }
        if (!matched) {
            init_printk("Phase 3a: section not found: %s", tg.name_key);
            init_fatal::halt(SRC_LOC());
        }
    }

    // ---- 4. 解禁 va_alloc ----
    g_va_alloc_base = align_up(kernel_vaddr_top, 0x200000);

    // ---- 5. kIMG 作为 movable 资产（不再专用映射——恒等窗口已覆盖） ----
    //      movable_file_entry_t{ base_ppn, size }：offset 0 起即文件内容，
    //      访问经恒等映射（[0x1000, dram_top)）/ Kspace_phyaddr_access_window。
    {
        movable_file_entry_t* kimg_desc = new movable_file_entry_t{
            .base_ppn = kimg_pbase >> 12,
            .size     = kelf_sz,
        };
        if (!asset_reg_add(asset_names::kimg, kimg_desc)) {
            init_printk("Phase 3a: asset dup: kimg");
            init_fatal::halt(SRC_LOC());
        }
    }

    init_printk("Phase 3a: done: kIMG(transient) %p size=%lu vaddr_top=%p",
                (void*)kimg_pbase, (unsigned long)kelf_sz, (void*)kernel_vaddr_top);

    // 资产树 dump（handoff 清单，字典序）
    if (g_asset_registry) {
        char line[512]; uint32_t o = 0;
        o += init_format(line + o, sizeof(line) - o, "Phase 3a: asset tree (%lu):",
                         (unsigned long)g_asset_registry->size());
        for (auto it = g_asset_registry->begin(); it != g_asset_registry->end(); ++it) {
            o += init_format(line + o, sizeof(line) - o, " [%s]", it->name);
        }
        init_printk("%s", line);
    }
    return 0;
}
