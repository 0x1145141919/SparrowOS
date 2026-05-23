#include "util/kptrace.h"
#include "util/kout.h"
#include "abi/os_error_definitions.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "abi/boot.h"
#ifdef USER_MODE
#include <dlfcn.h>
#endif
phyaddr_t ksymmanager::phybase;
 vaddr_t ksymmanager::virtbase;
 symbol_entry* ksymmanager::symbol_table;//虚拟地址
uint32_t ksymmanager::entry_count;
uint32_t ksymmanager::entry_size;
int ksymmanager::Init(vm_interval* entry,uint64_t file_size)
{
    phybase = entry->pbase();
    virtbase = entry->vbase();
    symbol_table = (symbol_entry*)virtbase;
    entry_size = file_size;
    entry_count = entry_size/sizeof(symbol_entry);
    return OS_SUCCESS;
}
symbol_entry *ksymmanager::get_entry_near_addr(vaddr_t addr)
{
    // 检查符号表是否有效
    if (!symbol_table || entry_count == 0) {
        return nullptr;
    }

    // 如果地址小于第一个符号的地址，没有符合条件的符号
    if (addr < symbol_table[0].address) {
        return nullptr;
    }

    uint32_t left = 0;
    uint32_t right = entry_count - 1;
    uint32_t result_idx = 0;

    // 二分查找最接近的地址（最后一个满足 symbol_table[i].address <= addr 的索引）
    while (left <= right) {
        uint32_t mid = left + (right - left) / 2;

        if (symbol_table[mid].address <= addr) {
            result_idx = mid;  // 当前是有效候选
            if (mid == UINT32_MAX || mid == entry_count - 1) {
                break; // 已达最大索引或已到最后一个条目
            }
            left = mid + 1; // 向右继续找更大的满足条件的索引
        } else {
            if (mid == 0) break; // 防止下溢
            right = mid - 1;     // 向左查找
        }
    }

    // 确保结果确实满足条件
    if (symbol_table[result_idx].address <= addr) {
        return &symbol_table[result_idx];
    }

    return nullptr;
}
phyaddr_t ksymmanager::get_phybase()
{
    return phybase;
}

vaddr_t ksymmanager::get_virtbase()
{
    return virtbase;
}

uint32_t ksymmanager::get_entry_count()
{
    return entry_count;
}

extern "C" void allthread_true_enter(void *(*entry)(void *), void *arg);

static inline bool str_eq(const char* lhs, const char* rhs)
{
    if(lhs == nullptr || rhs == nullptr){
        return false;
    }
    while(*lhs != '\0' && *rhs != '\0'){
        if(*lhs != *rhs){
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return (*lhs == '\0' && *rhs == '\0');
}

bool kptrace_current_stack_has_kthread_entry()
{
    struct StackFrame {
        StackFrame* rbp;
        uint64_t rip;
    };

    void* rbp_raw = nullptr;
    asm volatile ("movq %%rbp, %0" : "=r"(rbp_raw));
    StackFrame* frame = static_cast<StackFrame*>(rbp_raw);
    constexpr int MAX_FRAMES = 128;
    constexpr uint64_t kNearRangeBytes = 256;
    const uint64_t kthread_entry_addr = reinterpret_cast<uint64_t>(&allthread_true_enter);

    for(int i = 0; frame != nullptr && i < MAX_FRAMES; ++i){
        if((reinterpret_cast<uint64_t>(frame) & 0x7ull) != 0 || frame->rip == 0){
            return false;
        }

#ifdef KERNEL_MODE
        symbol_entry* sym = ksymmanager::get_entry_near_addr(frame->rip);
        if(sym != nullptr && str_eq(sym->name, "allthread_true_enter")){
            return true;
        }
#endif

        const uint64_t rip = frame->rip;
        const uint64_t diff = (rip >= kthread_entry_addr)
            ? (rip - kthread_entry_addr)
            : (kthread_entry_addr - rip);
        if(diff <= kNearRangeBytes){
            return true;
        }

        StackFrame* next = frame->rbp;
        if(next <= frame || (reinterpret_cast<uint64_t>(next) & 0x7ull) != 0){
            return false;
        }
        frame = next;
    }
    return false;
}

void self_trace()
{
    void* rbp;
    asm volatile ("movq %%rbp, %0" : "=r"(rbp));
    bsp_kout<< "self Trace:" << kendl;
    else_trace(rbp);
}

void else_trace(void* rbp)//递归,但是会忽略掉最近一层调用
{
    
    
    // 定义栈帧结构
    struct StackFrame {
        struct StackFrame* rbp;  // 前一个栈帧指针
        uint64_t rip;            // 返回地址
    };
    
    StackFrame* frame = static_cast<StackFrame*>(rbp);
    int frame_count = 0;
    const int MAX_FRAMES = 128;  // 限制回溯深度
    
    while (frame != nullptr && frame_count < MAX_FRAMES) {
        // 验证栈帧指针的合理性
        if ((uint64_t)frame % 8 != 0 || frame->rip == 0) {
            break;
        }
        
    bsp_kout<< "#" << frame_count << " RIP: " << (void*)(frame->rip);
#ifdef KERNEL_MODE
    // 获取靠近当前返回地址的符号
    symbol_entry* sym = ksymmanager::get_entry_near_addr(frame->rip);
    if (sym != nullptr) {
        bsp_kout<< " Symbol: " << sym->name << " (+" << (void*)(frame->rip - sym->address) << ")";
    }
#endif
#ifdef USER_MODE
    Dl_info info;
    if (dladdr((void*)frame->rip, &info) && info.dli_sname) {
        uintptr_t sym_addr = reinterpret_cast<uintptr_t>(info.dli_saddr);
        uintptr_t rip_addr = static_cast<uintptr_t>(frame->rip);
        bsp_kout<< " Symbol: " << info.dli_sname << " (+" << (void*)(rip_addr - sym_addr) << ")";
    }
#endif
    bsp_kout<< kendl;
        
        // 移动到下一个栈帧
        StackFrame* next_frame = frame->rbp;
        
        // 验证下一个栈帧的有效性，防止无限循环
        if (next_frame <= frame || (uint64_t)next_frame % 8 != 0) {
            break;
        }
        
        frame = next_frame;
        frame_count++;
    }
}
