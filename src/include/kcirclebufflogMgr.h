/**
 * 这是内核日志环形缓冲区模块头文件，负责把启动时的调试信息保存下来，易于启动后观察，gdb调试时查看。
 */
#include "stdint.h"
#include "util/lock.h"
#include "abi/boot.h"

// —— 时基（与 util/printk.h 同一约定）：返回【微秒】；未就绪返回 0 ——
// v2 环记录头要打 ts；各 ELF 各自提供符号（kernel 接 ktime / init 暂桩 0）。
extern "C" uint64_t now_ts_us();

// —— 记录头 magic 锚点（magic1→magic2 固定距离 13B，供扫描定位）——
static constexpr uint8_t LOG_REC_MAGIC1 = 0xA5;
static constexpr uint8_t LOG_REC_MAGIC2 = 0x5A;
 class DmesgRingBuffer
{
private:
    static char *buff;
    static uint64_t buffSize;
    static uint64_t tailIndex;
    static spinrwlock_cpp_t rwlock;
public:
    static void Init(vm_interval*logbuffer);
    static void putsk(char *str,uint64_t len_in_bytes);
};
// 紧凑布局（恰 17B）。注意：记录基址的 8 字节对齐是【环编排时】的不变量
// （record_add 负责把 tail_offset 上对齐到 8），不是结构体自身尺寸属性。
struct log_record_head_t{
uint16_t len;
uint8_t level;
uint8_t magic1;
uint32_t record_seq;
uint64_t ts_us;
uint8_t magic2;
} __attribute__((packed));
static_assert(sizeof(log_record_head_t) == 17, "log_record_head_t must be 17 bytes");
struct log_record_head_compressed
{
uint64_t ts_us;
uint32_t record_seq;
};

struct DmesgRingBuffer_soul{
    void *buff;
    uint64_t buffSize;
    uint64_t accumulate_mileage;//通过accumulate_mileage与buffSize做除法，余数是圈内，下一个可写引索，商则是累计回绕次数，整个变量也可以解释为游标移动总里程（单位字节）
    uint64_t accumulate_record_count;//累计这个里面塞了多少个记录
};//转生结构体
class DmesgRingBuffer_v2{
    private:
    DmesgRingBuffer_soul working_soul;
public:
    spinlock_cpp_t lock;
    void Reincarnate(DmesgRingBuffer_soul*soul);
    log_record_head_compressed record_add(uint16_t len,uint8_t level);//working_soul.tail_offset插入一个新的log_record_head_t，不过会自动从对齐为8的地址开始，并且会绕尾，而后再更新accumulate_record_count以及tail_offset，根据是否回绕选择性更新overwrite_count
    void putsk(char *str,uint16_t len_in_bytes);//从tail_offset开始向后写这个字符缓冲区，会自动处理回绕计数以及tail_offset
    const DmesgRingBuffer_soul*get_soul();//灵魂泄露出去，自己看着办拿不拿锁，比如panic时分不拿锁，要获取内核缓冲区的时候自己拿锁尽可能短的缓冲区快速搬走内存，而后锁外自己文本化解析
};
