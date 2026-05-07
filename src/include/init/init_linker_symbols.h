#ifndef INIT_LINKER_SYMBOLS_H
#define INIT_LINKER_SYMBOLS_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化内存布局常量
extern const uint64_t INIT_BASE;
extern const uint64_t INIT_IMAGE_SIZE;
extern const uint64_t INIT_STACK_SIZE;
extern const uint64_t INIT_HEAP_SIZE;

// 初始化代码段符号
extern uint8_t __init_text_start;
extern uint8_t __init_text_end;
extern const uint64_t __init_text_size;

// 初始化只读数据段符号
extern uint8_t __init_rodata_start;
extern uint8_t __init_rodata_end;
extern const uint64_t __init_rodata_size;

// 初始化数据段符号
extern uint8_t __init_data_start;
extern uint8_t __init_data_end;
extern const uint64_t __init_data_size;

// 初始化 BSS 段符号
extern uint8_t __init_bss_start;
extern uint8_t __init_bss_end;
extern const uint64_t __init_bss_size;

// 初始化栈段符号
extern uint8_t __init_stack_start;
extern uint8_t __init_stack_end;
extern const uint64_t __init_stack_size;

// 初始化堆段符号
extern uint8_t __init_heap_start;
extern uint8_t __init_heap_end;
extern const uint64_t __init_heap_size;

// 初始化镜像信息
extern const uint64_t __init_image_start;
extern const uint64_t __init_image_end;
extern const uint64_t __init_image_size;

// 获取地址的宏定义，方便使用
#define GET_INIT_SYMBOL_ADDR(symbol) ((void*)&symbol)

#ifdef __cplusplus
}
#endif

#endif // INIT_LINKER_SYMBOLS_H
