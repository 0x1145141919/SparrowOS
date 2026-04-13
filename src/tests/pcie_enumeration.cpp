#include "memory/AddresSpace.h"
#include "arch/x86_64/PCIe/prased.h"
#include "util/kout.h"
#include "firmware/ACPI_MCFG.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

// MCFG表签名
constexpr char MCFG_SIGNATURE[4] = {'M', 'C', 'F', 'G'};

int main(){
    int res = userspace_compatible_phymem_direct_map_enable();
    if(res != OS_SUCCESS){
        bsp_kout << "[ERROR] Failed to enable physical memory direct mapping\n";
        return -1;
    }
    
    bsp_kout.Init();
    
    // 打开并读取MCFG表
    const char* mcfg_path = "/sys/firmware/acpi/tables/MCFG";
    int fd = open(mcfg_path, O_RDONLY);
    if(fd < 0) {
        bsp_kout << "[ERROR] Cannot open " << mcfg_path << "\n";
        userspace_compatible_phymem_direct_map_disable();
        return -1;
    }
    
    // 获取文件大小
    struct stat st;
    if(fstat(fd, &st) < 0) {
        bsp_kout << "[ERROR] Cannot stat MCFG file\n";
        close(fd);
        userspace_compatible_phymem_direct_map_disable();
        return -1;
    }
    
    size_t file_size = static_cast<size_t>(st.st_size);
    bsp_kout << "[INFO] MCFG table size: " << file_size << " bytes\n";
    
    // 分配缓冲区并读取MCFG表内容
    uint8_t* mcfg_buffer = new uint8_t[file_size];
    if(mcfg_buffer == nullptr) {
        bsp_kout << "[ERROR] Failed to allocate buffer for MCFG table\n";
        close(fd);
        userspace_compatible_phymem_direct_map_disable();
        return -1;
    }
    
    ssize_t bytes_read = read(fd, mcfg_buffer, file_size);
    close(fd); // 读取完成后关闭文件描述符
    
    if(bytes_read < 0) {
        bsp_kout << "[ERROR] Failed to read MCFG table: " << strerror(errno) << "\n";
        delete[] mcfg_buffer;
        userspace_compatible_phymem_direct_map_disable();
        return -1;
    }
    
    if(static_cast<size_t>(bytes_read) != file_size) {
        bsp_kout << "[ERROR] Incomplete read: expected " << file_size 
                 << " bytes, got " << bytes_read << " bytes\n";
        delete[] mcfg_buffer;
        userspace_compatible_phymem_direct_map_disable();
        return -1;
    }
    
    bsp_kout << "[INFO] Successfully read " << bytes_read << " bytes from MCFG table\n";
    
    // 验证MCFG表头签名
    MCFG_Table* mcfg = reinterpret_cast<MCFG_Table*>(mcfg_buffer);
    
    // 检查签名是否为"MCFG"
    if(std::memcmp(mcfg->Header.Signature, MCFG_SIGNATURE, 4) != 0) {
        bsp_kout << "[ERROR] Invalid MCFG signature. Expected 'MCFG', got '";
        for(int i = 0; i < 4; i++) {
            bsp_kout << static_cast<char>(mcfg->Header.Signature[i]);
        }
        bsp_kout << "'\n";
        delete[] mcfg_buffer;
        userspace_compatible_phymem_direct_map_disable();
        return -1;
    }
    
    bsp_kout << "[INFO] MCFG signature verified: '";
    for(int i = 0; i < 4; i++) {
        bsp_kout << static_cast<char>(mcfg->Header.Signature[i]);
    }
    bsp_kout << "'\n";
    
    bsp_kout << "[INFO] MCFG table length: " << mcfg->Header.Length << " bytes\n";
    
    // 创建ecams_container_t实例
    global_container = new ecams_container_t(mcfg);
    
    if(global_container == nullptr || global_container->empty()) {
        bsp_kout << "[WARN] No ECAM segments found in MCFG table\n";
    } else {
        bsp_kout << "[INFO] Found " << global_container->size() << " ECAM segment(s)\n";
    }
    
    // 执行PCIe枚举
    pcie_text_praser();
    
    // 清理资源
    delete global_container;
    global_container = nullptr;
    
    delete[] mcfg_buffer;
    
    res = userspace_compatible_phymem_direct_map_disable();
    if(res != OS_SUCCESS) {
        bsp_kout << "[WARN] Failed to disable physical memory direct mapping\n";
    }
    
    return 0;
}