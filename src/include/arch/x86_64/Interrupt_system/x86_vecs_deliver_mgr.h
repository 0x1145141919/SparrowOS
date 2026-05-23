#include "Interrupt.h"
namespace Interrupt_module{
    constexpr uint8_t modloc_idt_mgr=1;
    namespace idt_mgr_events{ 
        namespace common_fail_reason_code{
            constexpr uint16_t INVALID_VEC=1;
            constexpr uint16_t INVALID_PROCESSOR_ID=2;
        }
        constexpr uint8_t init=0;
        namespace init_results{
            namespace fatal_reason_code{
                constexpr uint8_t SYMBOL_TABLE_UNAVAILABLE=1;
                constexpr uint8_t SYMBOL_TABLE_PARSE_FAIL=2;
                constexpr uint8_t NOT_ALL_IDT_FOUND=3;
            }
        };
        constexpr uint8_t alloc_vec=1;
        namespace alloc_vec_results{
            namespace fail_reason_code{
                constexpr uint16_t NO_FREE_VEC=3;
                constexpr uint16_t BAD_FUNC_PTR=4;
                constexpr uint16_t SYM_NOT_FOUND=5;
            }
        }
        constexpr uint8_t free_vec=2;
        namespace free_vec_results{
            namespace fail_reason_code{
                constexpr uint16_t VEC_NOT_ALLOCED=1;
            }
        }
        constexpr uint8_t get_vec=3;//这个只用common_fail_reason_code
        constexpr uint8_t vec_dispatch=4;
        namespace vec_dispatch_results{
            namespace fatal_reason_code{
                constexpr uint16_t BAD_VEC_RECIEVED=1;
            }
        }
    }
}
class idt_vec_dispatch_mgr{
    public:
    static KURD_t Init();//在MM_READY段被BSP调用
    static uint8_t alloc_vec(hard_interrupt_func_t func,uint32_t processor_id,KURD_t&kurd);//用OS_utils.h里面的is_addr_kernel_address函数确认
    static KURD_t free_vec(uint8_t vec,uint32_t processor_id);
    static hard_interrupt_func_t* get_vec(uint8_t vec,uint32_t processor_id,KURD_t& kurd);
};
class fred_based_mgr{

};
constexpr uint8_t INVALID_INTERRUPT_VEC=0xFF;
extern "C" uint8_t out_interrupt_vec_alloc(hard_interrupt_func_t func,uint32_t processor_id,KURD_t*kurd);
extern "C" KURD_t out_interrupt_vec_free(uint8_t vec,uint32_t processor_id);
extern "C" hard_interrupt_func_t* out_interrupt_vec_get(uint8_t vec,uint32_t processor_id,KURD_t* kurd);