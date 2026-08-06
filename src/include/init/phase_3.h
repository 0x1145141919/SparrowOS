#pragma once
#include "abi/boot.h"
#include "abi/src_loc.h"
#include "init/init_asset_registry.h"
#include "init/init_phase_ctx.h"
#include "init/init_fatal.h"
#include "init/init_linker_symbols.h"
#include "init/kernel_mmu.h"

// ============================================================================
// Phase 3a / Phase 3b — kernel.elf 装载与区间分配
// ============================================================================
// 原 init_init.cpp 内的两个串行阶段拆出到独立实现文件（phase_3a.cpp / phase_3b.cpp）。
// 共享状态：
//   - g_va_alloc_base：VA 分配器基址，唯一定义留在 init_init.cpp
//   - asset_reg_add：登记资产进 init 侧资产树（handoff 清单）

// VA 分配器基址（唯一定义在 init_init.cpp）
extern uint64_t g_va_alloc_base;

// Phase 3a (串行): kernel.elf 解包 → 精确狙击 4 段进 kmmu → 产出进资产容器
loc_code_t phase_3a_load_kernel(kernel_mmu* kmmu, const ctx_early_mem* em,
                                BootInfoHeader* header);

// Phase 3b (串行): 恒等映射 + 区间分配 + 架构信息收集 → 产出进资产容器 + ctx_intervals
loc_code_t phase_3b(kernel_mmu* kmmu, BootInfoHeader* header,
                    const ctx_early_mem* em, ctx_intervals* iv_out);

// 登记资产进 init 侧资产树（handoff 清单）。多arg name 的 arg1 指示路由类型，
// 树键 = arg0（本名）。返回 false 表示同名冲突（arg0 重复）。
inline bool asset_reg_add(const char* name, void* data) {
    if (!g_asset_registry) return false;
    // 浅拷贝账本铁律：registry 只存 name/data 指针值，栈上对象在作用域结束后必然悬垂。
    // 用 init.ld .stack 段链接符号做区间检查，命中即收尸（编程错误，不该静默吞掉）。
    const uintptr_t st_lo = (uintptr_t)&__init_stack_start;
    const uintptr_t st_hi = (uintptr_t)&__init_stack_end;
    const uintptr_t nm    = (uintptr_t)name;
    const uintptr_t dt    = (uintptr_t)data;
    if ((nm >= st_lo && nm < st_hi) || (dt >= st_lo && dt < st_hi)) {
        init_fatal::halt(SRC_LOC());
    }
    return g_asset_registry->add({ const_cast<char*>(name), data });
}
