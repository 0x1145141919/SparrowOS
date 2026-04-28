#pragma once
#include <block_device.h>
#include <util/Ktemplats.h>
#include <memory/AddresSpace.h>
class NVMeNameSpace:public BlockDevice{//除了get_sector_size，get_sector_count其它接口都是异步接口，必须在上下文中使用
    public:
    NVMeNameSpace(); //必须在内核线程中调用，涉及到异步操作，会被阻塞
    ~NVMeNameSpace() override;
    KURD_t read(uint64_t sector, uint32_t count, void *buf,uint64_t flags) override;
    KURD_t write(uint64_t sector, uint32_t count, void *buf,uint64_t flags) override;
    KURD_t flush(uint64_t flags) override;
    KURD_t discard(uint64_t sector, uint32_t count,uint64_t flags) override;
    uint64_t get_sector_count() const override;
    uint32_t get_sector_size() const override;
};
class NVMe_Controller{
    vm_interval bars[6];
    
    Ktemplats::list_doubly<NVMeNameSpace*> namespaces;
    NVMe_Controller(vaddr_t ecam);
};