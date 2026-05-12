#include "kcirclebufflogMgr.h"
#include "util/OS_utils.h"
extern "C"
{
    extern char* __klog_start;
    extern uint64_t __KLOG_SIZE;
}
char* DmesgRingBuffer::buff = nullptr;
uint64_t DmesgRingBuffer::buffSize = 0;
uint64_t DmesgRingBuffer::tailIndex = 0;
spinrwlock_cpp_t DmesgRingBuffer::rwlock;
void DmesgRingBuffer::Init(vm_interval*logbuffer)
{
    rwlock.write_lock();
    buff= (char*)logbuffer->vbase;
    tailIndex=0;
    buffSize = logbuffer->size;
    rwlock.write_unlock();
}

// 在文档2中更新putsk函数实现
void DmesgRingBuffer::putsk(char *str, uint64_t len_in_bytes) {
    interrupt_guard j;
    if (str == nullptr || len_in_bytes == 0) return;
    rwlock.write_lock();
    if (buffSize == 0) {
        rwlock.write_unlock();
        return;
    }

    // 处理超长数据（保留最后buffSize字节）
    if (len_in_bytes > buffSize) {
        str += (len_in_bytes - buffSize);
        len_in_bytes = buffSize;
    }

    // 计算尾部连续空间
    uint64_t remaining = buffSize - tailIndex;
    
    if (len_in_bytes <= remaining) {
        // 单次拷贝即可完成
        ksystemramcpy(str, buff + tailIndex, len_in_bytes);
        tailIndex += len_in_bytes;
        // 无需取模，因为remaining确保不会越界
    } else {
        // 分两段拷贝（跨越缓冲区末尾）
        ksystemramcpy(str, buff + tailIndex, remaining);
        uint64_t secondPart = len_in_bytes - remaining;
        ksystemramcpy(str + remaining, buff, secondPart);
        tailIndex = secondPart;  // 尾部指针回绕到开头
    }

    // 确保尾部指针不越界（当remaining恰好相等时）
    if (tailIndex >= buffSize) {
        tailIndex = 0;
    }
    rwlock.write_unlock();
}