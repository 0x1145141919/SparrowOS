#include "memory/kpoolmemmgr.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "util/OS_utils.h"
#include "memory/AddresSpace.h"
#include "memory/phygpsmemmgr.h"
#include "memory/FreePagesAllocator.h"
#include "util/kout.h"
#include "uutils/json_prasers.h"
#include <unistd.h>
#include <vector>
#include <random>
#include <pthread.h>
#include <cstring>

struct mem_seg
{
    uint64_t start_addr;//为1代表无效地址，其它4k对齐的地址都是有效地址
    uint64_t size;
    uint8_t page_state_replace;//表示页类型，0:KERNEL,1:USER_ANONYMOUS,2:USER_FILE,3:DMA
};
KURD_t test_basic_init() {
    bsp_kout<< "Testing basic initialization..." << kendl;
    uint64_t entry_count;
    phymem_segment* memory_map_ptr = load_and_parse_memory_map("/home/PS/PS_git/OS_pj_uefi/kernel/test_set/phy_instance_1.json", entry_count);
    uint64_t mem_max_phyaddr=memory_map_ptr[entry_count-1].start+memory_map_ptr[entry_count-1].size;
    mem_max_phyaddr=align_up(mem_max_phyaddr,4096);
    if(!memory_map_ptr){
        bsp_kout<< "Failed to load memory map." << kendl;
        _exit(-1);
    }
    loaded_VM_interval mem_map_interval={
        .pbase=0,
        .vbase=(vaddr_t)malloc(mem_max_phyaddr*sizeof(page)/4096),
        .size=mem_max_phyaddr*sizeof(page)/4096,
        .VM_interval_specifyid=VM_ID_MEM_MAP
    };
    init_to_kernel_header init_info{
        .phymem_segment_count = entry_count,
        .memory_map = memory_map_ptr,
        .loaded_VM_interval_count=1,
        .loaded_VM_intervals=&mem_map_interval,
        
    };
    KURD_t status = all_pages_arr::Init(&init_info);
    if (!success_all_kurd(status)) {
        bsp_kout<< "phymemspace_mgr::Init() failed with status: ";
        bsp_kout<< status;
        bsp_kout<< kendl;
        return status;
    }
    status=FreePagesAllocator::Init();
    if(error_kurd(status)){
        bsp_kout<< "FreePagesAllocator::Init() failed with status: "<<kendl;
        return status;
    }
    status=FreePagesAllocator::second_stage(FreePagesAllocator::DEFAULT_THREAD);
    bsp_kout<< "Basic initialization test passed." << kendl;
    return status;
}


// 压力测试函数
KURD_t stress_test_page_allocation_and_deallocation() {
    bsp_kout<< "Starting stress test for page allocation and deallocation..." << kendl;
    
    // 生成随机mem_seg向量
    std::vector<mem_seg> allocations;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::lognormal_distribution<double> size_dist(log(1<<14), 1.0); // 平均约16KB大小

    const int num_allocations = 10000; // 生成10000个测试项
    bsp_kout<< "Generating " << num_allocations << " random allocations..." << kendl;

    for (int i = 0; i < num_allocations; ++i) {
        mem_seg seg;
        seg.start_addr = 1; // 按照注释初始化为无效地址
        // 使用对数正态分布生成大小，并转换为uint64_t类型，限制最大值为256MB
        uint64_t size = static_cast<uint64_t>(size_dist(gen)) % 0x10000000 + 1;
        seg.size = size; // 使用对数正态分布生成的size
        // 随机选择页面类型 (0:KERNEL,1:USER_ANONYMOUS,2:USER_FILE,3:DMA)
        seg.page_state_replace = static_cast<uint8_t>(gen() % 4);
        allocations.push_back(seg);
    }

    // 创建状态机进行100万次分配和释放操作
    std::uniform_int_distribution<int> index_dist(0, num_allocations - 1);
    bsp_kout<< "Starting state machine with 1,000,000 allocation/deallocation cycles..." << kendl;
    uint64_t old_stamp=rdtsc();
    
    for (int cycle = 0; cycle < 1000000; ++cycle) {
        // 随机选择一个索引
        int idx = index_dist(gen);
        
        // 获取对应的mem_seg
        mem_seg& seg = allocations[idx];
        
        if (seg.start_addr == 1) {
            // 地址无效，进行分配
            // 根据page_state_replace确定页面类型
            page_state_t page_type;
            switch(seg.page_state_replace) {
                case 0: page_type = page_state_t::kernel_anonymous; break;
                case 1: page_type = page_state_t::user_anonymous; break;
                case 2: page_type = page_state_t::user_file; break;
                case 3: page_type = page_state_t::dma; break;
                default: page_type = page_state_t::kernel_anonymous; break; // 默认使用KERNEL
            }
            
            // 计算需要的页数（向上取整）
            uint64_t page_count = (seg.size + 0xFFF) >> 12; // 4KB页大小
            
            Alloc_result res = FreePagesAllocator::alloc(page_count<<12,BUDDY_ALLOC_DEFAULT_FLAG, page_type);
            
            if (res.result.result == result_code::SUCCESS && res.base != 0) {
                seg.start_addr = res.base; // 更新为实际分配的地址
            } else {
                bsp_kout<< "Allocation failed at cycle " << cycle 
                             << ", size=" << seg.size << kendl;
            }
        } else {
            // 地址有效，进行释放
            // 计算页数
            uint64_t page_count = (seg.size + 0xFFF) >> 12;
            
            KURD_t free_result = FreePagesAllocator::free(seg.start_addr, page_count*4096);
            
            if (free_result.result == result_code::SUCCESS) {
                seg.start_addr = 1; // 重置为无效地址
            } else {
                bsp_kout<< "Deallocation failed at cycle " << cycle 
                             << ", addr=0x" << seg.start_addr << kendl;
            }
        }
        
        // 每隔100000次输出一次进度和时间
        if ((cycle + 1) % 100000 == 0) {
            bsp_kout<<DEC<< "Completed " << (cycle + 1) << " cycles" << kendl;
            uint64_t now_tsc=rdtsc();
            bsp_kout<< "Elapsed time: " << (now_tsc-old_stamp)/100000 << " tsc avg per ops" << kendl;
            old_stamp=now_tsc;
            bsp_kout<< "Timestamp: " <<now<< kendl;
        }
    }
    
    bsp_kout<< "=== Stress Test Completed ===" << kendl;

    return KURD_t();
}

constexpr uint32_t THREAD_STATS_INTERVAL = 100000;  // 统计间隔
constexpr uint32_t MAX_STATS_ENTRIES = 20;  // 最多存储 20 个时间戳（对应 200 万次循环）

// 线程参数结构体
struct thread_test_params {
    uint32_t thread_id;
    int num_allocations;         // 每个线程生成的测试项数量
    int num_cycles;              // 每个线程的循环次数
    std::vector<mem_seg>* allocations;  // 指向线程私有测试集的指针
    
    // 统计数据数组
    uint64_t tsc_timestamps[MAX_STATS_ENTRIES];  // 每 100k次操作的 TSC 时间戳
    uint32_t stats_count;        // 实际存储的时间戳数量
    uint64_t alloc_success_count;  // 分配成功次数
    uint64_t alloc_fail_count;     // 分配失败次数
    uint64_t free_success_count;   // 释放成功次数
    uint64_t free_fail_count;      // 释放失败次数
};

// 单线程压力测试函数（由每个线程调用）
void* stress_test_thread(void* arg) {
    thread_test_params* params = (thread_test_params*)arg;
    
    // 初始化统计数据
    params->stats_count = 0;
    params->alloc_success_count = 0;
    params->alloc_fail_count = 0;
    params->free_success_count = 0;
    params->free_fail_count = 0;
    for (uint32_t i = 0; i < MAX_STATS_ENTRIES; ++i) {
        params->tsc_timestamps[i] = 0;
    }
    
    // 生成随机 mem_seg 向量（每个线程独立生成）
    std::random_device rd;
    std::mt19937 gen(rd() + params->thread_id);  // 使用线程 ID 作为种子偏移
    std::lognormal_distribution<double> size_dist(log(1<<14), 1.0); // 平均约 16KB 大小

    for (int i = 0; i < params->num_allocations; ++i) {
        mem_seg seg;
        seg.start_addr = 1; // 按照注释初始化为无效地址
        // 使用对数正态分布生成大小，并转换为 uint64_t 类型，限制最大值为 256MB
        uint64_t size = static_cast<uint64_t>(size_dist(gen)) % 0x10000000 + 1;
        seg.size = size; // 使用对数正态分布生成的 size
        // 随机选择页面类型 (0:KERNEL,1:USER_ANONYMOUS,2:USER_FILE,3:DMA)
        seg.page_state_replace = static_cast<uint8_t>(gen() % 4);
        params->allocations->push_back(seg);
    }

    // 创建状态机进行分配和释放操作
    std::uniform_int_distribution<int> index_dist(0, params->num_allocations - 1);
    uint64_t old_stamp = rdtsc();
    
    uint64_t alloc_success_count = 0;
    uint64_t free_success_count = 0;
    uint64_t alloc_fail_count = 0;
    uint64_t free_fail_count = 0;
    
    for (int cycle = 0; cycle < params->num_cycles; ++cycle) {
        // 随机选择一个索引
        int idx = index_dist(gen);
        
        // 获取对应的 mem_seg
        mem_seg& seg = (*params->allocations)[idx];
        
        if (seg.start_addr == 1) {
            // 地址无效，进行分配
            KURD_t alloc_result;
            // 根据 page_state_replace 确定页面类型
            page_state_t page_type;
            switch(seg.page_state_replace) {
                case 0: page_type = page_state_t::kernel_anonymous; break;
                case 1: page_type = page_state_t::user_anonymous; break;
                case 2: page_type = page_state_t::user_file; break;
                case 3: page_type = page_state_t::dma; break;
                default: page_type = page_state_t::kernel_anonymous; break; // 默认使用 KERNEL
            }
            
            // 计算需要的页数（向上取整）
            uint64_t page_count = (seg.size + 0xFFF) >> 12; // 4KB 页大小
            
            Alloc_result res = FreePagesAllocator::alloc(page_count<<12,BUDDY_ALLOC_DEFAULT_FLAG, page_type);
            
            if (res.result.result == result_code::SUCCESS && res.base != 0) {
                seg.start_addr = res.base; // 更新为实际分配的地址
                alloc_success_count++;
            } else {
                alloc_fail_count++;
            }
        } else {
            // 地址有效，进行释放
            // 计算页数
            uint64_t page_count = (seg.size + 0xFFF) >> 12;
            
            KURD_t free_result = FreePagesAllocator::free(seg.start_addr, page_count*4096);
            
            if (free_result.result == result_code::SUCCESS) {
                seg.start_addr = 1; // 重置为无效地址
                free_success_count++;
            } else {
                free_fail_count++;
            }
        }
        
        // 每隔 100000 次记录一次性能数据到统计数组
        if ((cycle + 1) % 100000 == 0 && params->stats_count < MAX_STATS_ENTRIES) {
            uint64_t now_tsc = rdtsc();
            params->tsc_timestamps[params->stats_count] = now_tsc - old_stamp;
            params->stats_count++;
            old_stamp = now_tsc;
        }
    }
    
    // 更新参数结构体中的统计信息供主线程汇总
    params->alloc_success_count = alloc_success_count;
    params->alloc_fail_count = alloc_fail_count;
    params->free_success_count = free_success_count;
    params->free_fail_count = free_fail_count;

    return nullptr;
}

KURD_t stress_test_page_allocation_and_deallocation(uint32_t thread_count) {
    bsp_kout << "Starting multi-threaded stress test with " << thread_count 
             << " threads..." << kendl;
    
    pthread_t* threads = new pthread_t[thread_count];
    thread_test_params* params = new thread_test_params[thread_count];
    std::vector<std::vector<mem_seg>> all_allocations(thread_count);
    
    // 配置每个线程的参数
    for (uint32_t i = 0; i < thread_count; ++i) {
        params[i].thread_id = i;
        params[i].num_allocations = 10000;  // 每个线程生成 10000 个测试项
        params[i].num_cycles = 1000000;     // 每个线程执行 100 万次循环
        params[i].allocations = &all_allocations[i];
    }
    
    uint64_t start_time = rdtsc();
    
    // 创建并启动所有线程
    for (uint32_t i = 0; i < thread_count; ++i) {
        pthread_create(&threads[i], nullptr, stress_test_thread, &params[i]);
    }
    
    // 等待所有线程完成
    for (uint32_t i = 0; i < thread_count; ++i) {
        pthread_join(threads[i], nullptr);
    }
    
    uint64_t end_time = rdtsc();
    
    // 打印所有线程的统计信息
    bsp_kout << "=== All Threads Completed ===" << kendl;
    bsp_kout << "Total elapsed time: " << (end_time - start_time) / 1000000 
             << " M tsc" << kendl << kendl;
    
    for (uint32_t i = 0; i < thread_count; ++i) {
        bsp_kout << "[Thread " << params[i].thread_id << "] === Thread Test Results ===" << kendl;
        bsp_kout << "[Thread " << params[i].thread_id << "] Alloc success: " 
                 << params[i].alloc_success_count 
                 << ", fail: " << params[i].alloc_fail_count << kendl;
        bsp_kout << "[Thread " << params[i].thread_id << "] Free success: " 
                 << params[i].free_success_count 
                 << ", fail: " << params[i].free_fail_count << kendl;
        
        // 打印性能统计
        if (params[i].stats_count > 0) {
            bsp_kout << "[Thread " << params[i].thread_id << "] Performance (per " 
                     << THREAD_STATS_INTERVAL << " ops):" << kendl;
            for (uint32_t j = 0; j < params[i].stats_count; ++j) {
                bsp_kout << "  Interval " << (j + 1) << ": " 
                         << params[i].tsc_timestamps[j] / 100000 << "  tsc" << kendl;
            }
        }
        bsp_kout << kendl;
    }
    
    // 清理资源
    delete[] threads;
    delete[] params;
    // all_allocations 会自动析构

    return KURD_t();
}

int main(int argc, char** argv) {
    bsp_kout.Init();
    bsp_kout << "Starting comprehensive phymemspace_mgr tests..." << kendl;
    bsp_kout<<DEC;
    // 解析命令行参数获取线程数
    uint32_t thread_count =22;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            thread_count = atoi(argv[++i]);
        }
    }
    
    bsp_kout << "Using " << thread_count << " threads for stress test" << kendl;
    
    // 基本初始化测试
    KURD_t status = test_basic_init();
    if (!success_all_kurd(status)) {
        bsp_kout << "Basic init test failed with status: ";
        bsp_kout << status;
        bsp_kout << kendl;
        return -1;
    }    
    
    // 执行多线程压力测试
    status = stress_test_page_allocation_and_deallocation(thread_count);
    if (!success_all_kurd(status)) {
        bsp_kout << "Stress test failed with status: ";
        bsp_kout << status;
        bsp_kout << kendl;
        return -1;
    }
    //FreePagesAllocator::print_all_bcb_statistics();
    return 0;
}