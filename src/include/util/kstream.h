#pragma once
#include "abi/os_error_definitions.h"
#include "util/OS_utils.h"
#include "util/lock.h"
struct tmp_buff_statistics_t
{
    // ===== 输出类型调用统计 =====
    uint64_t calls_str;             // operator<<(const char*)
    uint64_t calls_char;            // operator<<(char)
    uint64_t calls_ptr;             // operator<<(const void*)
    uint64_t calls_u8;
    uint64_t calls_s8;
    uint64_t calls_u16;
    uint64_t calls_s16;
    uint64_t calls_u32;
    uint64_t calls_s32;
    uint64_t calls_u64;
    uint64_t calls_s64;
    uint64_t calls_KURD;
    // ===== 控制/状态类调用 =====
    uint64_t calls_shift_bin;
    uint64_t calls_shift_dec;
    uint64_t calls_shift_hex;
};
struct udp_style_backend {
    char name[64];
    void (*running_stage_write)(const char* buf, uint64_t len);
    void (*running_stage_putchar)(char c);
    void (*running_stage_num)(uint64_t raw,num_format_t format,numer_system_select radix);
};
struct syn_thread_backend{//线程阻塞输出接口里面可能有自己的缓冲区，在线程上下文视角就是程序指针从uniform_puts出来的时候就是逻辑发出去了
    char name[64];
    void (*uniform_puts)(const char* buf, uint64_t len);
    void (*flush)();
};
struct syn_mechain_backend{//汇编指令层的阻塞接口，也就是说可以所有场景使用，但是时间自负
    char name[64];
    void (*uniform_puts)(const char* buf, uint64_t len);
};
class kout;
class tmp_buff{
    //一般是栈上分配的临时缓冲区，用于多线程下统一给kout输出
    private:
    numer_system_select num_sys;
    enum entry_type_t:uint8_t {
        character,
        num,str,time,KURD
    };
    struct entry_t{
        entry_type_t entry_type;
        num_format_t num_type;//i8,u8,i16,u16,i32,u32,i64,u64,(float,double)浮点类型不支持
        numer_system_select num_sys;
        uint32_t str_len;
        union 
        {
            uint64_t data;
            char*str;
            char character;
            KURD_t kurd;
        }data;
        entry_t():entry_type(entry_type_t::character),num_type(num_format_t::u8),num_sys(numer_system_select::DEC),str_len(0),data{}{
        }
    };
    uint16_t entry_count;
    entry_t*entry_array;
    uint16_t entry_top;
    friend kout;
    public:
    tmp_buff_statistics_t*statisitics;
    tmp_buff& operator<<(KURD_t info);
    tmp_buff& operator<<(const char* str);
    tmp_buff& operator<<(char c);
    tmp_buff& operator<<(const void* ptr);
    tmp_buff& operator<<(uint64_t num);
    tmp_buff& operator<<(int64_t num);
    tmp_buff& operator<<(uint32_t num);
    tmp_buff& operator<<(int32_t num);
    tmp_buff& operator<<(uint16_t num);
    tmp_buff& operator<<(int16_t num);
    tmp_buff& operator<<(uint8_t num);
    tmp_buff& operator<<(int8_t num);
    tmp_buff& operator<<(numer_system_select radix);
    tmp_buff(uint16_t entry_max);//初始化时entry_array既可以栈上分配也可以堆上分配，外部控制，但是每个场合根据自己的场景
    //构造的时候一律是内部进制选择为DEC为初始状态，需要传入相应的控制字控制是否构造statisitcs结构体，以及控制位控制内部的缓冲区/统计字段是堆上/栈上分配。
    void discard();//外部输入是通过内部entry_array作为一个数组从引索0向上累积的，甩给kstream打印的时候则是根据顺序从0向上打印，这个接口就是重新置0。每个kout里面坚决不能自己主动discard，很明显是主动调用以实现一个buff复用
    ~tmp_buff();
    bool is_full();
};
class kout
{
    private:   
    spinlock_cpp_t lock;
    public:
    kout_backend*backends={0};//放public就是让各位自己的场景自己安排引索，此数组直接堆上分配
    uint64_t calls_tmp_buff;
    kout& operator<<(tmp_buff& tmp_buff);
    kout(uint16_t backends_count);
    ~kout(){};
};
