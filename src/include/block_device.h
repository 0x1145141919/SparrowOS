#pragma once
#include <cstdint>
#include "abi/os_error_definitions.h"
class BlockDevice {
    uint32_t sector_size;
    uint64_t sector_count;
    uint64_t one_time_write_limit;
    uint64_t one_time_read_limit;
    public:
    virtual ~BlockDevice() = default;
    virtual KURD_t read(uint64_t sector, uint32_t count, void *buf,uint64_t flags) = 0;//传入虚拟地址
    virtual KURD_t write(uint64_t sector, uint32_t count, void *buf,uint64_t flags) = 0;//传入虚拟地址
    virtual KURD_t flush(uint64_t flags)=0; // 可选默认实现
    virtual KURD_t discard(uint64_t sector, uint32_t count,uint64_t flags)=0; // TRIM
    virtual uint64_t get_sector_count() const = 0;
    virtual uint32_t get_sector_size() const = 0;
};