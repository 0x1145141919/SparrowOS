#pragma once
#include "abi/os_error_definitions.h"
#include "util/OS_utils.h"
#include "util/lock.h"
namespace kio
{
class now_time
{
    private:  
    public:now_time(){};
};
class endl
{
    private:
    public:endl(){};
};
enum radix_shift_t:uint8_t {
    BIN_shift=0,
    DEC_shift=1,
    HEX_shift=2
};
struct kout_backend {
    char name[64];
    uint64_t is_masked:1;
    uint64_t reserved:63;
    void (*running_stage_write)(const char* buf, uint64_t len);
    void (*running_stage_putchar)(char c);
    void (*running_stage_num)(uint64_t raw,num_format_t format,numer_system_select radix);
    void (*panic_write)(const char* buf, uint64_t len);
    void (*early_write)(const char* buf, uint64_t len);
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
    struct entry{
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
        entry():entry_type(entry_type_t::character),num_type(num_format_t::u8),num_sys(numer_system_select::DEC),str_len(0),data(0){
        }
    };
    static constexpr uint16_t entry_max=64;
    entry entry_array[entry_max];
    uint16_t entry_top;
    friend kout;
    public:
    tmp_buff& operator<<(KURD_t info);
    tmp_buff& operator<<(const char* str);
    tmp_buff& operator<<(char c);
    tmp_buff& operator<<(const void* ptr);
    tmp_buff& operator<<(uint64_t num);
    tmp_buff& operator<<(int64_t num);
    tmp_buff& operator<<(uint32_t num);
    tmp_buff& operator<<(int32_t num);
    tmp_buff& operator<<(now_time time);
    tmp_buff& operator<<(endl end);
    tmp_buff& operator<<(uint16_t num);
    tmp_buff& operator<<(int16_t num);
    tmp_buff& operator<<(uint8_t num);
    tmp_buff& operator<<(int8_t num);
    tmp_buff& operator<<(numer_system_select radix);
    tmp_buff();//初始化时entry_array栈上分配，设计上是栈用，没有多线程安全
    ~tmp_buff();
    bool is_full();
};
class kout
{
    public:
    struct kout_statistics_t
    {
    uint64_t total_printed_chars;
        //还有各种入口统计处,主要是对不同类型的<<重载调用次数的统计
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
    uint64_t calls_now_time;        // operator<<(now_time)
    uint64_t calls_tmp_buff;
    // ===== 控制/状态类调用 =====
    uint64_t calls_shift_bin;
    uint64_t calls_shift_dec;
    uint64_t calls_shift_hex;

    // ===== 行为统计 =====
    uint64_t explicit_endl;         // 通过 endl / kendl 结束的行

    // ===== 未来预留 =====
    //uint64_t reserved[8];
    };
    protected:    
    #ifdef USER_MODE
    bool is_print_to_stdout;
    bool is_print_to_stderr;
    #endif
    kout_statistics_t statistics;
    
    numer_system_select curr_numer_system;
    static constexpr char hex_chars[16]={
        '0','1','2','3','4','5','6','7',
        '8','9','A','B','C','D','E','F'
    };
    // 打印KURD_t中level字段对应的字符串表示
    void __print_level_code(KURD_t value);
    // 打印KURD_t中module_code字段对应的字符串表示
    void __print_module_code(KURD_t value);
    // 打印KURD_t中result_code字段对应的字符串表示
    void __print_result_code(KURD_t value);
    // 打印KURD_t中err_domain字段对应的字符串表示
    void __print_err_domain(KURD_t value);
    static void __print_event_hex(uint8_t event_code);
    static void __print_memmodule_kurd(KURD_t kurd);
    static void (*top_module_KURD_interpreter[256])(KURD_t info);
    /**
     * @brief 输出数字
     * 
     * @param num_ptr 数字的指针
     * @param numer_system 数字系统,准确说是进制，比如二进制，十进制，十六进制
     * @param len_in_bytes 数字的字节长度，合法长度只有1,2,4,8
     * @param is_signed 是否是带符号的数字，只有在DEC状态下判断是否是复数，然后进行打印，BIN/HEX统一当作无符号数打印
     */
    
    void print_numer(
        uint64_t*num_ptr,
        numer_system_select numer_system,
        uint8_t len_in_bytes,
        bool is_signed);
    static constexpr uint16_t MAX_STRING_LEN=4096;
    static constexpr uint16_t MAX_BACKEND_COUNT=64;
    kout_backend*backends[MAX_BACKEND_COUNT]={0};
    void uniform_puts(const char* str,uint64_t len);
    void raw_puts_and_count(const char* str,uint64_t len);
    spinlock_cpp_t lock;
    public:
    kout& operator<<(tmp_buff& tmp_buff);
    kout& operator<<(KURD_t info);
    kout& operator<<(const char* str);
    kout& operator<<(char c);
    kout& operator<<(const void* ptr);
    kout& operator<<(uint64_t num);
    kout& operator<<(int64_t num);
    kout& operator<<(uint32_t num);
    kout& operator<<(int32_t num);
    kout& operator<<(now_time time);
    kout& operator<<(endl end);
    kout& operator<<(uint16_t num);
    kout& operator<<(int16_t num);
    kout& operator<<(uint8_t num);
    kout& operator<<(int8_t num);
    kout& operator<<(numer_system_select radix);
    uint64_t register_backend(kout_backend backend);//返回~0表示分配失败
    bool unregister_backend(uint64_t index);
    bool mask_backend(uint64_t index);
    void shift_bin();
    void shift_dec();
    void shift_hex();
    kout_statistics_t get_statistics();
    void Init();
    ~kout(){};
};


void defalut_KURD_module_interpator(KURD_t kurd);
}
extern kio::endl kendl;
extern kio::now_time now;
extern kio::kout bsp_kout;
