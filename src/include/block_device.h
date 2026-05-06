#pragma once
#include <cstdint>
#include "abi/os_error_definitions.h"
struct BlockDeviceOps;
struct BlockDevice {
    uint32_t block_device_type;
    uint32_t sector_size;
    uint64_t sector_count;
    uint64_t one_time_write_limit;
    uint64_t one_time_read_limit;
    BlockDeviceOps* ops;
    void *private_data;//各ID的私有数据
};
struct pbuf_t{
    uint64_t pbase;
    uint64_t size;
};
struct LBA_interval_t{
    uint64_t start;
    uint64_t LBA_count;
};
struct BlockDeviceOps{
    KURD_t (*read)(BlockDevice*dev,uint64_t sector, uint32_t count, void *buf,uint64_t flags);
    KURD_t (*write)(BlockDevice*dev,uint64_t sector, uint32_t count, void *buf,uint64_t flags);
    KURD_t (*flush)(BlockDevice*dev,uint64_t flags);
    KURD_t (*discard)(BlockDevice*dev,uint64_t sector, uint32_t count,uint64_t flags);
};
