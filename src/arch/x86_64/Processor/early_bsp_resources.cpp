#include "abi/os_error_definitions.h"
// [GS_COMPLEX_TODO] early_bsp_resources 待 GS 复合体重写
extern "C" int ap_regist_core(uint32_t processor_id)
{
    (void)processor_id;
    return 0;
}
