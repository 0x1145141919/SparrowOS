#include "util/kout.h"
#include "arch/x86_64/Interrupt_system/Interrupt.h"
#include "kcirclebufflogMgr.h"
#include "arch/x86_64/core_hardwares/PortDriver.h"
#include "util/OS_utils.h"
#include "ktime.h"
#include "panic.h"
#include "memory/memmodule_err_definitions.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "memory/AddresSpace.h"
#include "memory/kpoolmemmgr.h"
#include "memory/FreePagesAllocator.h"
#include "memory/all_pages_arr.h"
#ifdef USER_MODE
#include <cstring> 
#include <unistd.h>
 #endif
kio::kout bsp_kout;
kio::endl kendl;
kio::now_time now;
void (*kio::kout::top_module_KURD_interpreter[256]) (KURD_t info);
void kio::defalut_KURD_module_interpator(KURD_t kurd)
{
    result_t result={
        .kernel_result=kurd
    };
    bsp_kout<<"default_KURD_module_interpator the raw:"<<HEX<<result.raw<<DEC<<kendl;
}


void kio::kout::__print_event_hex(uint8_t event_code)
{
    bsp_kout.raw_puts_and_count("[event:0x", 9);
    uint64_t code = event_code;
    bsp_kout.print_numer(&code, HEX, 1, false);
    bsp_kout.raw_puts_and_count("]", 1);
}

void kio::kout::__print_memmodule_kurd(KURD_t kurd)
{
    auto put = [](const char* s) {
        bsp_kout.raw_puts_and_count(s, (uint64_t)strlen_in_kernel(s));
    };
    switch (kurd.in_module_location) {
        case MEMMODULE_LOCAIONS::LOCATION_CODE_ADDRESSPACE:
            put("[mem_loc:ADDRESSPACE]");
            switch (kurd.event_code) {
                case MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_INIT:
                    put("[event:INIT]"); break;
                case MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_ENABLE_VMENTRY:
                    put("[event:ENABLE_VMENTRY]"); break;
                case MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_DISABLE_VMENTRY:
                    put("[event:DISABLE_VMENTRY]"); break;
                case MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_TRAN_TO_PHY:
                    put("[event:TRAN_TO_PHY]"); break;
                case MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_INVALIDATE_TLB:
                    put("[event:INVALIDATE_TLB]"); break;
                case MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_BUILD_INDENTITY_MAP_ONLY_ON_gKERNELSPACE:
                    put("[event:BUILD_INDENTITY_MAP_ONLY_ON_gKERNELSPACE]"); break;
                case MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_UNREGIST:
                    put("[event:UNREGIST]"); break;
                default:
                    __print_event_hex(kurd.event_code); break;
            }
            break;
        case MEMMODULE_LOCAIONS::LOCATION_CODE_KSPACE_MAP_MGR:
            put("[mem_loc:KSPACE_MAP_MGR]");
            switch (kurd.event_code) {
                case MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_INIT:
                    put("[event:INIT]"); break;
                case MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_ENABLE_VMENTRY:
                    put("[event:ENABLE_VMENTRY]"); break;
                case MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_DISABLE_VMENTRY:
                    put("[event:DISABLE_VMENTRY]"); break;
                case MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_TRAN_TO_PHY_ENTRY:
                    put("[event:TRAN_TO_PHY_ENTRY]"); break;
                case MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_INVALIDATE_TLB:
                    put("[event:INVALIDATE_TLB]"); break;
                case MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_SET:
                    put("[event:PAGES_SET]"); break;
                case MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_CLEAR:
                    put("[event:PAGES_CLEAR]"); break;
                case MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_SEG_TO_INFO_PACKAGE:
                    put("[event:SEG_TO_INFO_PACKAGE]"); break;
                case MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_VM_SEARCH_BY_ADDR:
                    put("[event:VM_SEARCH_BY_ADDR]"); break;
                case MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_UNREGIST:
                    put("[event:UNREGIST]"); break;
                default:
                    __print_event_hex(kurd.event_code); break;
            }
            break;
        case MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR:
            put("[mem_loc:KPOOLMEMMGR]");
            switch (kurd.event_code) {
                case MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_INIT:
                    put("[event:INIT]"); break;
                case MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_ALLOC:
                    put("[event:ALLOC]"); break;
                case MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_REALLOC:
                    put("[event:REALLOC]"); break;
                case MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_PER_PROCESSOR_HEAP_INIT:
                    put("[event:PER_PROCESSOR_HEAP_INIT]"); break;
                default:
                    __print_event_hex(kurd.event_code); break;
            }
            break;
        case MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB:
            put("[mem_loc:KPOOLMEMMGR_HCB]");
            switch (kurd.event_code) {
                case MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INIT:
                    put("[event:INIT]"); break;
                case MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_CLEAR:
                    put("[event:CLEAR]"); break;
                case MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_ALLOC:
                    put("[event:ALLOC]"); break;
                case MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_FREE:
                    put("[event:FREE]"); break;
                case MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INHEAP_REALLOC:
                    put("[event:INHEAP_REALLOC]"); break;
                default:
                    __print_event_hex(kurd.event_code); break;
            }
            break;
        case MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB_BITMAP:
            put("[mem_loc:KPOOLMEMMGR_HCB_BITMAP]");
            switch (kurd.event_code) {
                case MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_BITMAP_EVENTS::EVENT_CODE_INIT:
                    put("[event:INIT]"); break;
                default:
                    __print_event_hex(kurd.event_code); break;
            }
            break;
        case MEMMODULE_LOCAIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR:
            put("[mem_loc:FREEPAGES_ALLOCATOR]");
            switch (kurd.event_code) {
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::EVENT_CODE_INIT:
                    put("[event:INIT]"); break;
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::EVENT_CODE_INIT_SECOND_STAGE:
                    put("[event:INIT_SECOND_STAGE]"); break;
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::EVENT_CODE_ALLOC:
                    put("[event:ALLOC]"); break;
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::EVENT_CODE_FREE:
                    put("[event:FREE]"); break;
                default:
                    __print_event_hex(kurd.event_code); break;
            }
            break;
        case MEMMODULE_LOCAIONS::LOCATION_CODE_TRANSPARNENT_PAGE:
            put("[mem_loc:TRANSPARNENT_PAGE]");
            switch (kurd.event_code) {
                case MEMMODULE_LOCAIONS::TRANSPARNENT_PAGE_EVENTS::EVENT_CODE_SPILT:
                    put("[event:SPILT]"); break;
                case MEMMODULE_LOCAIONS::TRANSPARNENT_PAGE_EVENTS::EVENT_CODE_MERGE:
                    put("[event:MERGE]"); break;
                case MEMMODULE_LOCAIONS::TRANSPARNENT_PAGE_EVENTS::EVENT_CODE_MERGE_FREE:
                    put("[event:MERGE_FREE]"); break;
                default:
                    __print_event_hex(kurd.event_code); break;
            }
            break;
        case MEMMODULE_LOCAIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK:
            put("[mem_loc:FREEPAGES_ALLOCATOR_BCB]");
            switch (kurd.event_code) {
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_INIT:
                    put("[event:INIT]"); break;
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_ALLOCATE_BUDY_WAY:
                    put("[event:ALLOCATE_BUDY_WAY]"); break;
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_CONANICO_FREE:
                    put("[event:CONANICO_FREE]"); break;
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_SPLIT_PAGE:
                    put("[event:SPLIT_PAGE]"); break;
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_FLUSH_FREE_COUNT:
                    put("[event:FLUSH_FREE_COUNT]"); break;
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_TOP_FOLD:
                    put("[event:TOP_FOLD]"); break;
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_FREE_PAGES_FLUSH:
                    put("[event:FREE_PAGES_FLUSH]"); break;
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_FREE:
                    put("[event:FREE]"); break;
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_REPLAY_VALIDATE:
                    put("[event:REPLAY_VALIDATE]"); break;
                default:
                    __print_event_hex(kurd.event_code); break;
            }
            break;
        case MEMMODULE_LOCAIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_BITMAP:
            put("[mem_loc:FREEPAGES_ALLOCATOR_BCB_BITMAP]");
            switch (kurd.event_code) {
                case MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_BITMAP::EVENT_CODE_INIT:
                    put("[event:INIT]"); break;
                default:
                    __print_event_hex(kurd.event_code); break;
            }
            break;
        case MEMMODULE_LOCAIONS::LOCATION_CODE_KSPACE_MAP_MGR_VMENTRY_RBTREE:
            put("[mem_loc:KSPACE_MAP_MGR_VMENTRY_RBTREE]");
            __print_event_hex(kurd.event_code);
            break;
        case MEMMODULE_LOCAIONS::LOCATION_CODE_KSPACE_MAP_MGR_PGS_PAGE_TABLE:
            put("[mem_loc:KSPACE_MAP_MGR_PGS_PAGE_TABLE]");
            __print_event_hex(kurd.event_code);
            break;
        case MEMMODULE_LOCAIONS::LOCATION_CODE_BASE_MEMMGR:
            put("[mem_loc:BASE_MEMMGR]");
            __print_event_hex(kurd.event_code);
            break;
        case MEMMODULE_LOCAIONS::LOCATION_CODE_PHYMEM_ACCESSOR:
            put("[mem_loc:PHYMEM_ACCESSOR]");
            __print_event_hex(kurd.event_code);
            break;
        case MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES:
            put("[mem_loc:OUT_SURFACES]");
            switch (kurd.event_code) {
                case 0:
                    put("[event:PAGES_VALLOC]"); break;
                case 1:
                    put("[event:PAGES_VFREE]"); break;
                case 2:
                    put("[event:PAGES_ALLOC]"); break;
                case 3:
                    put("[event:PAGES_FREE]"); break;
                case 4:
                    put("[event:KEYWORD_NEW]"); break;
                case 5:
                    put("[event:KEYWORD_DELETE]"); break;
                default:
                    __print_event_hex(kurd.event_code); break;
            }
            break;
        default:
            put("[mem_loc:unknown]");
            __print_event_hex(kurd.event_code);
            break;
    }
}

void kio::kout::print_numer(
    uint64_t *num_ptr, 
    numer_system_select numer_system, 
    uint8_t len_in_bytes, 
    bool is_signed)
{
    #ifdef KERNEL_MODE
    if (GlobalKernelStatus >= SCHEDUL_READY && GlobalKernelStatus != PANIC) {
        num_format_t format = num_format_t::u64;
        if (is_signed) {
            switch (len_in_bytes) {
                case 1: format = num_format_t::s8; break;
                case 2: format = num_format_t::s16; break;
                case 4: format = num_format_t::s32; break;
                default: format = num_format_t::s64; break;
            }
        } else {
            switch (len_in_bytes) {
                case 1: format = num_format_t::u8; break;
                case 2: format = num_format_t::u16; break;
                case 4: format = num_format_t::u32; break;
                default: format = num_format_t::u64; break;
            }
        }

        uint64_t raw = 0;
        switch (len_in_bytes) {
            case 1: raw = is_signed
                ? static_cast<uint64_t>(*reinterpret_cast<int8_t*>(num_ptr))
                : static_cast<uint64_t>(*reinterpret_cast<uint8_t*>(num_ptr)); break;
            case 2: raw = is_signed
                ? static_cast<uint64_t>(*reinterpret_cast<int16_t*>(num_ptr))
                : static_cast<uint64_t>(*reinterpret_cast<uint16_t*>(num_ptr)); break;
            case 4: raw = is_signed
                ? static_cast<uint64_t>(*reinterpret_cast<int32_t*>(num_ptr))
                : static_cast<uint64_t>(*reinterpret_cast<uint32_t*>(num_ptr)); break;
            default: raw = is_signed
                ? static_cast<uint64_t>(*reinterpret_cast<int64_t*>(num_ptr))
                : *reinterpret_cast<uint64_t*>(num_ptr); break;
        }

        for (uint64_t i = 0; i < MAX_BACKEND_COUNT; i++) {
            kout_backend* backend = backends[i];
            if (!backend || backend->is_masked) continue;
            if (backend->running_stage_num) {
                backend->running_stage_num(raw, format, numer_system);
            }
        }
        return;
    } 
    #endif
    char buf[70];          // 足够覆盖 64bit BIN + 符号
    char out[70];
    uint32_t idx = 0;

    uint64_t value = 0;

    // ===== 加载数值 =====
    switch (len_in_bytes) {
        case 1: value = *(uint8_t*)num_ptr; break;
        case 2: value = *(uint16_t*)num_ptr; break;
        case 4: value = *(uint32_t*)num_ptr; break;
        case 8: value = *(uint64_t*)num_ptr; break;
        default:
            return;
    }

    // ===== 处理 DEC 下的符号 =====
    bool negative = false;
    if (numer_system == DEC && is_signed) {
        int64_t signed_val = 0;
        switch (len_in_bytes) {
            case 1: signed_val = *(int8_t*)num_ptr; break;
            case 2: signed_val = *(int16_t*)num_ptr; break;
            case 4: signed_val = *(int32_t*)num_ptr; break;
            case 8: signed_val = *(int64_t*)num_ptr; break;
        }
        if (signed_val < 0) {
            negative = true;
            value = (uint64_t)(-signed_val);
        }
    }

    // ===== 特殊情况：0 =====
    if (value == 0) {
        buf[idx++] = '0';
    } else {
        uint32_t base = 10;
        if (numer_system == BIN) base = 2;
        else if (numer_system == HEX) base = 16;

        while (value > 0) {
            uint32_t digit = value % base;
            value /= base;

            if (base == 16)
                buf[idx++] = hex_chars[digit];
            else
                buf[idx++] = '0' + digit;
        }
    }

    // ===== 负号 =====
    if (negative) {
        buf[idx++] = '-';
    }
    
    // ===== 反向输出 =====
    for (uint32_t i = 0; i < idx; ++i) {
        out[i] = buf[idx - 1 - i];
    }
    
    #ifdef KERNEL_MODE
        // EARLY/MM_READY/PANIC 统一走字符串路径并按状态机后端分发。
        uniform_puts(out, idx);
    #endif
    
    #ifdef USER_MODE
    if (is_print_to_stdout) write(1, out, idx);
    if (is_print_to_stderr) write(2, out, idx);
    #endif
    statistics.total_printed_chars += idx;
}

void kio::kout::__print_level_code(KURD_t value)
{
    switch (value.level)
    {
        case level_code::INVALID:
            raw_puts_and_count("[level:INVALID]", sizeof("[level:INVALID]") - 1);
            break;
        case level_code::INFO:
            raw_puts_and_count("[level:INFO]", sizeof("[level:INFO]") - 1);
            break;
        case level_code::NOTICE:
            raw_puts_and_count("[level:NOTICE]", sizeof("[level:NOTICE]") - 1);
            break;
        case level_code::WARNING:
            raw_puts_and_count("[level:WARNING]", sizeof("[level:WARNING]") - 1);
            break;
        case level_code::ERROR:
            raw_puts_and_count("[level:ERROR]", sizeof("[level:ERROR]") - 1);
            break;
        case level_code::FATAL:
            raw_puts_and_count("[level:FATAL]", sizeof("[level:FATAL]") - 1);
            break;
        default:
            raw_puts_and_count("[level:unknown]", sizeof("[level:unknown]") - 1);
            break;
    }
}

void kio::kout::__print_module_code(KURD_t value)
{
    switch (value.module_code)
    {
        case module_code::INVALID:
            raw_puts_and_count("[module_code:INVALID]", sizeof("[module_code:INVALID]") - 1);
            break;
        case module_code::MEMORY:
            raw_puts_and_count("[module_code:MEMORY]", sizeof("[module_code:MEMORY]") - 1);
            break;
        case module_code::SCHEDULER:
            raw_puts_and_count("[module_code:SCHEDULER]", sizeof("[module_code:SCHEDULER]") - 1);
            break;
        case module_code::INTERRUPT:
            raw_puts_and_count("[module_code:INTERRUPT]", sizeof("[module_code:INTERRUPT]") - 1);
            break;
        case module_code::FIRMWARE:
            raw_puts_and_count("[module_code:FIRMWARE]", sizeof("[module_code:FIRMWARE]") - 1);
            break;
        case module_code::VFS:
            raw_puts_and_count("[module_code:VFS]", sizeof("[module_code:VFS]") - 1);
            break;
        case module_code::VMM:
            raw_puts_and_count("[module_code:VMM]", sizeof("[module_code:VMM]") - 1);
            break;
        case module_code::INFRA:
            raw_puts_and_count("[module_code:INFRA]", sizeof("[module_code:INFRA]") - 1);
            break;
        case module_code::DEVICES:
            raw_puts_and_count("[module_code:DEVICES]", sizeof("[module_code:DEVICES]") - 1);
            break;
        case module_code::DEVICES_CORE:
            raw_puts_and_count("[module_code:DEVICES_CORE]", sizeof("[module_code:DEVICES_CORE]") - 1);
            break;
        case module_code::HARDWARE_DEBUG:
            raw_puts_and_count("[module_code:HARDWARE_DEBUG]", sizeof("[module_code:HARDWARE_DEBUG]") - 1);
            break;
        case module_code::USER_KERNEL_ABI:
            raw_puts_and_count("[module_code:USER_KERNEL_ABI]", sizeof("[module_code:USER_KERNEL_ABI]") - 1);
            break;
        case module_code::TIME:
            raw_puts_and_count("[module_code:TIME]", sizeof("[module_code:TIME]") - 1);
            break;
        case module_code::PANIC:
            raw_puts_and_count("[module_code:PANIC]", sizeof("[module_code:PANIC]") - 1);
            break;
        default:
            raw_puts_and_count("[module_code:unknown]", sizeof("[module_code:unknown]") - 1);
            break;
    }
}

void kio::kout::__print_result_code(KURD_t value)
{
    switch (value.result)
    {
        case result_code::SUCCESS:
            raw_puts_and_count("[result:SUCCESS]", sizeof("[result:SUCCESS]") - 1);
            break;
        case result_code::SUCCESS_BUT_SIDE_EFFECT:
            raw_puts_and_count("[result:SUCCESS_BUT_SIDE_EFFECT]", sizeof("[result:SUCCESS_BUT_SIDE_EFFECT]") - 1);
            break;
        case result_code::PARTIAL_SUCCESS:
            raw_puts_and_count("[result:PARTIAL_SUCCESS]", sizeof("[result:PARTIAL_SUCCESS]") - 1);
            break;
        case result_code::FAIL:
            raw_puts_and_count("[result:FAIL]", sizeof("[result:FAIL]") - 1);
            break;
        case result_code::RETRY:
            raw_puts_and_count("[result:RETRY]", sizeof("[result:RETRY]") - 1);
            break;
        case result_code::FATAL:
            raw_puts_and_count("[result:FATAL]", sizeof("[result:FATAL]") - 1);
            break;
        default:
            raw_puts_and_count("[result:unknown]", sizeof("[result:unknown]") - 1);
            break;
    }
}

void kio::kout::__print_err_domain(KURD_t value)
{
    switch (value.domain)
    {
        case err_domain::INVALID:
            raw_puts_and_count("[err_domain:INVALID]", sizeof("[err_domain:INVALID]") - 1);
            break;
        case err_domain::CORE_MODULE:
            raw_puts_and_count("[err_domain:CORE_MODULE]", sizeof("[err_domain:CORE_MODULE]") - 1);
            break;
        case err_domain::ARCH:
            raw_puts_and_count("[err_domain:ARCH]", sizeof("[err_domain:ARCH]") - 1);
            break;
        case err_domain::USER:
            raw_puts_and_count("[err_domain:USER]", sizeof("[err_domain:USER]") - 1);
            break;
        case err_domain::HYPERVISOR:
            raw_puts_and_count("[err_domain:HYPERVISOR]", sizeof("[err_domain:HYPERVISOR]") - 1);
            break;
        case err_domain::OUT_MODULES:
            raw_puts_and_count("[err_domain:OUT_MODULES]", sizeof("[err_domain:OUT_MODULES]") - 1);
            break;
        case err_domain::FILE_SYSTEM:
            raw_puts_and_count("[err_domain:FILE_SYSTEM]", sizeof("[err_domain:FILE_SYSTEM]") - 1);
            break;
        case err_domain::HARDWARE:
            raw_puts_and_count("[err_domain:HARDWARE]", sizeof("[err_domain:HARDWARE]") - 1);
            break;
        default:
            raw_puts_and_count("[err_domain:unknown]", sizeof("[err_domain:unknown]") - 1);
            break;
    }
}

void kio::kout::uniform_puts(const char *str, uint64_t len)
{
    if (!str || len == 0) return;
    switch(GlobalKernelStatus){
        case ENTER:
        break;//enter状态实质上什么也打印不了
        case EARLY_BOOT:
        case PANIC_WILL_ANALYZE:
        case MM_READY:
        //遍历所有backend选择非空指针并且没有被maked的early_write
        {
            for(uint64_t i = 0; i < MAX_BACKEND_COUNT; i++){
                kout_backend* backend = backends[i];
                if(!backend || backend->is_masked) continue;
                if(backend->early_write) backend->early_write(str, len);
            }
        }
        break;
        case SCHEDUL_READY:
        //遍历所有backend选择非空指针并且没有被maked的running_stage_write
        {
            for(uint64_t i = 0; i < MAX_BACKEND_COUNT; i++){
                kout_backend* backend = backends[i];
                if(!backend || backend->is_masked) continue;
                if(backend->running_stage_write) backend->running_stage_write(str, len);
            }
        }
        break;
        case PANIC:
        //遍历所有backend选择非空指针并且没有被maked的panic_write
        {
            for(uint64_t i = 0; i < MAX_BACKEND_COUNT; i++){
                kout_backend* backend = backends[i];
                if(!backend || backend->is_masked) continue;
                if(backend->panic_write) backend->panic_write(str, len);
            }
        }
        break;
        
    }

}

void kio::kout::raw_puts_and_count(const char *str, uint64_t len)
{
    if (!str || len == 0) return;
    uniform_puts(str, len);
    statistics.calls_str++;
    statistics.total_printed_chars += len;
}

kio::kout &kio::kout::operator<<(tmp_buff& tmp_buff)
{
    if(GlobalKernelStatus!=PANIC)spinlock_interrupt_about_guard guard(lock);
    statistics.calls_tmp_buff++;

    uint16_t limit = tmp_buff.entry_top;
    if (limit > tmp_buff::entry_max) {
        limit = tmp_buff::entry_max;
    }

    for (uint16_t i = 0; i < limit; ++i) {
        const tmp_buff::entry& e = tmp_buff.entry_array[i];
        switch (e.entry_type) {
            case tmp_buff::entry_type_t::str: {
                if (e.data.str) {
                    uint32_t len = e.str_len;
                    raw_puts_and_count(e.data.str, len);
                } else {
                    raw_puts_and_count("(null)", 6);
                }
                break;
            }
            case tmp_buff::entry_type_t::character: {
                #ifdef KERNEL_MODE
                uniform_puts(&e.data.character, 1);
                #endif
                #ifdef USER_MODE
                if (is_print_to_stdout) write(1, &e.data.character, 1);
                if (is_print_to_stderr) write(2, &e.data.character, 1);
                #endif
                statistics.calls_char++;
                statistics.total_printed_chars++;
                break;
            }
            case tmp_buff::entry_type_t::num: {
                bool is_signed = false;
                uint8_t len = 8;
                switch (e.num_type) {
                    case num_format_t::u8:  statistics.calls_u8++;  len = 1; break;
                    case num_format_t::s8:  statistics.calls_s8++;  len = 1; is_signed = true; break;
                    case num_format_t::u16: statistics.calls_u16++; len = 2; break;
                    case num_format_t::s16: statistics.calls_s16++; len = 2; is_signed = true; break;
                    case num_format_t::u32: statistics.calls_u32++; len = 4; break;
                    case num_format_t::s32: statistics.calls_s32++; len = 4; is_signed = true; break;
                    case num_format_t::u64: statistics.calls_u64++; len = 8; break;
                    case num_format_t::s64: statistics.calls_s64++; len = 8; is_signed = true; break;
                    default: break;
                }

                if (is_signed) {
                    if (len == 1) {
                        int8_t v = (int8_t)e.data.data;
                        print_numer((uint64_t*)&v, e.num_sys, len, true);
                    } else if (len == 2) {
                        int16_t v = (int16_t)e.data.data;
                        print_numer((uint64_t*)&v, e.num_sys, len, true);
                    } else if (len == 4) {
                        int32_t v = (int32_t)e.data.data;
                        print_numer((uint64_t*)&v, e.num_sys, len, true);
                    } else {
                        int64_t v = (int64_t)e.data.data;
                        print_numer((uint64_t*)&v, e.num_sys, len, true);
                    }
                } else {
                    if (len == 1) {
                        uint8_t v = (uint8_t)e.data.data;
                        print_numer((uint64_t*)&v, e.num_sys, len, false);
                    } else if (len == 2) {
                        uint16_t v = (uint16_t)e.data.data;
                        print_numer((uint64_t*)&v, e.num_sys, len, false);
                    } else if (len == 4) {
                        uint32_t v = (uint32_t)e.data.data;
                        print_numer((uint64_t*)&v, e.num_sys, len, false);
                    } else {
                        uint64_t v = (uint64_t)e.data.data;
                        print_numer(&v, e.num_sys, len, false);
                    }
                }
                break;
            }
            case tmp_buff::entry_type_t::time: {
                statistics.calls_now_time++;
                #ifdef KERNEL_MODE
                    raw_puts_and_count("[", 1);
                    miusecond_time_stamp_t stamp = ktime::get_microsecond_stamp();
                    print_numer(&stamp, DEC, 8, false);
                    raw_puts_and_count("]", 1);
                #endif
                #ifdef USER_MODE
                raw_puts_and_count("<tsc=", sizeof("<tsc=") - 1);
                uint64_t tsc = rdtsc();
                print_numer(&tsc, DEC, 8, false);
                raw_puts_and_count(">", 1);
                #endif
                break;
            }
            case tmp_buff::entry_type_t::KURD: {
                statistics.calls_KURD++;
                __print_level_code(e.data.kurd);
                __print_result_code(e.data.kurd);
                __print_err_domain(e.data.kurd);
                __print_module_code(e.data.kurd);
                top_module_KURD_interpreter[e.data.kurd.module_code](e.data.kurd);
                break;
            }
            default:
                break;
        }
    }
    return *this;
}

kio::kout &kio::kout::operator<<(KURD_t info)
{
    if(GlobalKernelStatus!=PANIC)spinlock_interrupt_about_guard guard(lock);
    statistics.calls_KURD++;
    __print_level_code(info);
    __print_result_code(info);
    __print_err_domain(info);
    __print_module_code(info);
    top_module_KURD_interpreter[info.module_code](info);
    return *this;
}
kio::kout &kio::kout::operator<<(const char *str)
    
{
    if(GlobalKernelStatus!=PANIC)
    spinlock_interrupt_about_guard guard(lock);
    int strlength = strlen_in_kernel(str);
    #ifdef KERNEL_MODE
    
    uniform_puts(str, strlength);
    #endif
    #ifdef USER_MODE
    if (is_print_to_stdout) write(1, str, strlength);
    if (is_print_to_stderr) write(2, str, strlength);
    #endif
    statistics.calls_str++;
    statistics.total_printed_chars += strlength;
    return *this;
}

kio::kout &kio::kout::operator<<(char c)
{
    if(GlobalKernelStatus!=PANIC)spinlock_interrupt_about_guard guard(lock);
    #ifdef KERNEL_MODE
    uniform_puts(&c, 1);
    #endif
    #ifdef USER_MODE
    if (is_print_to_stdout) write(1, &c, 1);
    if (is_print_to_stderr) write(2, &c, 1);
    #endif
    statistics.calls_char++;
    statistics.total_printed_chars++;
    return *this;
}

void kio::kout::Init()
{
    ksetmem_8(&statistics, 0, sizeof(statistics));
    for(int i = 0; i < 256; i++){
        top_module_KURD_interpreter[i]=defalut_KURD_module_interpator;
    }
    top_module_KURD_interpreter[module_code::MEMORY]=__print_memmodule_kurd;
    
    #ifdef KERNEL_MODE
    kout_backend dmesg_buffer_handlers={
        .name="dmesg_buffer",
        .is_masked=0,
        .reserved=0,        
        .running_stage_write=nullptr,
        .panic_write=&DmesgRingBuffer::putsk,
        .early_write=&DmesgRingBuffer::putsk,
    };
    register_backend(dmesg_buffer_handlers);
    #endif
    #ifdef USER_MODE
    is_print_to_stdout = true;
    is_print_to_stderr = false;
    #endif
}
kio::kout &kio::kout::operator<<(const void *ptr)
{
    if(GlobalKernelStatus!=PANIC)spinlock_interrupt_about_guard guard(lock);
    #ifdef KERNEL_MODE
    uniform_puts("0x", 2);
    #endif
    #ifdef USER_MODE
    if (is_print_to_stdout) write(1, "0x", 2);
    if (is_print_to_stderr) write(2, "0x", 2);
    #endif
    uint64_t address = (uint64_t)ptr;
    print_numer(&address, HEX, sizeof(void *), false);
    statistics.calls_ptr++;
    return *this;
}
kio::kout &kio::kout::operator<<(uint64_t num)
{
    if(GlobalKernelStatus!=PANIC)spinlock_interrupt_about_guard guard(lock);
    statistics.calls_u64++;
    print_numer(&num, curr_numer_system, 8, false);
    return *this;
}

kio::kout &kio::kout::operator<<(int64_t num)
{
    spinlock_interrupt_about_guard guard(lock);
    statistics.calls_s64++;
    print_numer((uint64_t*)&num, curr_numer_system, 8, true);
    return *this;
}

kio::kout &kio::kout::operator<<(uint32_t num)
{
    if(GlobalKernelStatus!=PANIC)spinlock_interrupt_about_guard guard(lock);
    statistics.calls_u32++;
    print_numer((uint64_t*)&num, curr_numer_system, 4, false);
    return *this;
}

kio::kout &kio::kout::operator<<(int32_t num)
{
    if(GlobalKernelStatus!=PANIC)spinlock_interrupt_about_guard guard(lock);
    statistics.calls_s32++;
    print_numer((uint64_t*)&num, curr_numer_system, 4, true);
    return *this;
}

kio::kout &kio::kout::operator<<(uint16_t num)
{
    if(GlobalKernelStatus!=PANIC)spinlock_interrupt_about_guard guard(lock);
    statistics.calls_u16++;
    print_numer((uint64_t*)&num, curr_numer_system, 2, false);
    return *this;
}

kio::kout &kio::kout::operator<<(int16_t num)
{
    if(GlobalKernelStatus!=PANIC)spinlock_interrupt_about_guard guard(lock);
    statistics.calls_s16++;
    print_numer((uint64_t*)&num, curr_numer_system, 2, true);
    return *this;
}

kio::kout &kio::kout::operator<<(uint8_t num)
{
    if(GlobalKernelStatus!=PANIC)spinlock_interrupt_about_guard guard(lock);
    statistics.calls_u8++;
    print_numer((uint64_t*)&num, curr_numer_system, 1, false);
    return *this;
}

kio::kout &kio::kout::operator<<(int8_t num)
{
    if(GlobalKernelStatus!=PANIC)spinlock_interrupt_about_guard guard(lock);
    statistics.calls_s8++;
    print_numer((uint64_t*)&num, curr_numer_system, 1, true);
    return *this;
}

void kio::kout::shift_bin()
{
    curr_numer_system = BIN;
}

void kio::kout::shift_dec()
{
    curr_numer_system = DEC;
}

void kio::kout::shift_hex()
{
    curr_numer_system = HEX;
}

kio::kout::kout_statistics_t kio::kout::get_statistics()
{
    spinlock_interrupt_about_guard guard(lock);
    return statistics;
}

kio::kout &kio::kout::operator<<(now_time time)
{
    spinlock_interrupt_about_guard guard(lock);
    statistics.calls_now_time++;
    #ifdef KERNEL_MODE 
    raw_puts_and_count("<", 1);
    miusecond_time_stamp_t stamp=ktime::get_microsecond_stamp();
    print_numer(&stamp, DEC, 8, false);
    raw_puts_and_count(">", 1);
    #endif
    #ifdef USER_MODE
    raw_puts_and_count("<tsc=", sizeof("<tsc=") - 1);
    uint64_t tsc=rdtsc();
    print_numer(&tsc, DEC, 8, false);
    raw_puts_and_count(">", 1);
    #endif
    return *this;
}

kio::kout &kio::kout::operator<<(endl end)
{
    if(GlobalKernelStatus!=PANIC)spinlock_interrupt_about_guard guard(lock);
    statistics.explicit_endl++;
    raw_puts_and_count("\n", 1);
    return *this;
}
uint64_t kio::kout::register_backend(kout_backend backend)
{
    for(uint64_t i = 0; i < MAX_BACKEND_COUNT; i++){
        if(!backends[i]){
            backends[i]=new kout_backend;
            *backends[i]=backend;
            return i;
        }
    }
    return ~0;
}
bool kio::kout::unregister_backend(uint64_t index)
{
    if(index>=MAX_BACKEND_COUNT)return false;
    if(backends[index]){
        delete backends[index];
        return true;
    }
    return false;
}
bool kio::kout::mask_backend(uint64_t index)
{
    if(index>=MAX_BACKEND_COUNT)return false;
    if(backends[index]){
        if(backends[index]->is_masked){
            backends[index]->is_masked=false;
            return true;
        }
    }
    return false;
}

kio::kout &kio::kout::operator<<(numer_system_select radix)
{   
     if(GlobalKernelStatus!=PANIC)spinlock_interrupt_about_guard guard(lock);
     switch (radix) {
        case BIN: shift_bin(); statistics.calls_shift_bin++; break;
        case DEC: shift_dec(); statistics.calls_shift_dec++; break;
        case HEX: shift_hex(); statistics.calls_shift_hex++; break;
        default: shift_dec(); statistics.calls_shift_dec++; break;
    }
    return *this;
}
