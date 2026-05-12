/**
 * 这是内核日志环形缓冲区模块头文件，负责把启动时的调试信息保存下来，易于启动后观察，gdb调试时查看。
 */
#include "stdint.h"
#include "util/lock.h"
#include "abi/boot.h"
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
