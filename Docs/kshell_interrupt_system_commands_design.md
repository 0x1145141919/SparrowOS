# 内核 Shell (kshell) 中断系统诊断命令设计文档

## 📋 文档概述

本文档描述了内核态 shell（kshell）中**中断系统**相关的诊断命令设计规范。这些命令用于查询和管理 IDT、IOAPIC、处理器 GS 槽等中断相关资源。

**核心定位**：
- **运行环境**：内核态线程，通过 i8042 键盘驱动输入，bsp_kout 输出到 GOP/串口
- **主要用途**：中断系统调试、IDT 管理、IOAPIC 配置、GS 槽操作
- **设计约束**：
  - 禁止动态内存分配（早期初始化阶段）
  - 统一使用 `bsp_kout` 输出
  - 整数运算代替浮点运算
  - 使用 `enum class` 定义命令类型
  - 头文件常量使用 `constexpr`

**⚠️ 安全特性**：
- 只读命令无副作用，无需确认
- 写操作命令必须有严格确认机制
- 参数验证必须严格

---

## 🎯 命令分类与优先级

### Phase 1 - 处理器和 IDT 管理 ⭐⭐⭐⭐⭐

#### 1. `cpucount` - CPU 数量查询

**功能描述**：
显示系统中已注册和激活的 CPU 核心数量，包括 BSP 和 AP 的信息。

**数据来源/操作接口**：
```cpp
class x86_smp_processors_container {
private:
    static constexpr uint32_t max_processor_count = 4096;
    static uint32_t total_processor_count;
    static uint32_t bsp_apic_id;
    
public:
    static x64_local_processor* get_currunt_mgr();
    static x64_local_processor* get_processor_mgr_by_processor_id(prcessor_id_t id);
    static x64_local_processor* get_processor_mgr_by_apic_id(x2apicid_t apic_id);
};
```

**命令语法**：
```
cpucount [detail_level]

参数：
  [detail_level]  - 可选，详细程度（默认 normal）
                    brief=简要, normal=标准, full=完整

示例：
  cpucount             # 标准输出
  cpucount brief       # 简要信息
  cpucount full        # 完整信息（包括每个核心的详细信息）
```

**输出示例**：
```
=== CPU Count Information ===

Total Processors:    12
BSP APIC ID:         0
Max Supported:       4096

Processor List:
  ID   APIC ID   Status
  ---  --------  ------
  0    0         Active (BSP)
  1    1         Active
  2    2         Active
  3    3         Active
  4    4         Active
  5    5         Active
  6    6         Active
  7    7         Active
  8    8         Active
  9    9         Active
  10   10        Active
  11   11        Active

Summary:
  Active Cores:    12
  Inactive Cores:  0
```

**实现要点**：
- 遍历 `local_processor_interrupt_mgr_array` 数组
- 统计已注册的核心数量
- 显示 BSP 和 AP 的区分
- 支持不同的详细程度

---

#### 2. `idtstat` - IDT 状态查询

**功能描述**：
显示当前处理器的 IDT（中断描述符表）状态，包括已注册的中断向量、处理程序分布等。

**数据来源/操作接口**：
```cpp
class x64_local_processor {
private:
    IDTEntry idt[256];
    Ktemplats::kernel_bitmap* handler_register_bitmap;
    constexpr static uint8_t allocatable_handler_count = 192; 
    constexpr static uint8_t allocatable_handler_base = 32; 
    
public:
    bool handler_register(uint8_t vector, void* handler);
    uint8_t handler_alloc(void* handler);
    bool handler_unregister(uint8_t vector);
    void unsafe_handler_register_without_vecnum_check(uint8_t vector, void* handler);
    void unsafe_handler_unregister_without_vecnum_check(uint8_t vector);
};
```

**命令语法**：
```
idtstat [processor_id] [mode]

参数：
  [processor_id]  - 可选，处理器 ID（默认当前处理器）
  [mode]          - 可选，显示模式（默认 summary）
                    summary=摘要, vectors=向量详情, handlers=处理程序列表

示例：
  idtstat              # 当前处理器摘要
  idtstat 0            # 处理器 0 的 IDT
  idtstat 0 vectors    # 处理器 0 的向量详情
```

**输出示例（摘要）**：
```
=== IDT Status (Processor 0) ===

IDT Overview:
  Total Entries:     256
  Registered:        45
  Available:         147
  Reserved:          64 (0-31)

Handler Distribution:
  Vectors 0-31:      Reserved (CPU exceptions)
  Vectors 32-223:    Allocatable (192 slots)
  Vectors 224-255:   Reserved

Registration Rate:   23.4% (45/192)

Bitmap Status:
  Used Bits:         45
  Free Bits:         147
```

**输出示例（向量详情）**：
```
=== IDT Vector Details (Processor 0) ===

Registered Vectors:
  Vector  Handler Address    Type
  ------  -----------------  -----------
  32      0xFFFFFFFF80123456 IRQ Handler
  33      0xFFFFFFFF80123789 IRQ Handler
  40      0xFFFFFFFF80124ABC Timer IRQ
  48      0xFFFFFFFF80125DEF Device IRQ
  ...

Vector Ranges:
  32-39:   ████████░░░░░░░░░░░░ 8/16 used
  40-47:   ████░░░░░░░░░░░░░░░░ 4/16 used
  48-55:   ██░░░░░░░░░░░░░░░░░░ 2/16 used
  ...

Note: Only showing allocatable range (32-223).
```

**实现要点**：
- 读取 `handler_register_bitmap` 统计已注册的向量
- 显示向量分布直方图
- 支持指定处理器 ID
- 区分保留向量和可分配向量

---

#### 3. `idtreg <vector> <handler>` - IDT 处理程序注册

**功能描述**：
在指定中断向量上注册处理程序。**危险操作**，可能影响系统稳定性。

**数据来源/操作接口**：
```cpp
class x64_local_processor {
public:
    // 安全接口（推荐使用）
    bool handler_register(uint8_t vector, void* handler);
    uint8_t handler_alloc(void* handler);  // 自动分配向量
    
    // 不安全接口（仅供高级用户使用）
    void unsafe_handler_register_without_vecnum_check(uint8_t vector, void* handler);
};
```

**命令语法**：
```
idtreg <vector> <handler_addr> [-f]

参数：
  <vector>        - 中断向量号（32-223，十进制或十六进制）
  <handler_addr>  - 处理程序地址（十六进制，必须 0x 前缀）
  [-f]            - 可选，强制注册，跳过确认提示

示例：
  idtreg 40 0xFFFFFFFF80123456      # 注册向量 40
  idtreg 0x28 0xFFFFFFFF80124ABC    # 十六进制向量
  idtreg 50 0xFFFFFFFF80125DEF -f   # 强制注册
```

**输出示例**：
```
=== IDT Handler Registration ===
⚠️  WARNING: This operation modifies the IDT!

Request:
  Processor:       Current (ID: 0)
  Vector:          40 (0x28)
  Handler:         0xFFFFFFFF80123456

Current Status:
  Vector 40:       Unregistered

⚠️  Registering a handler will replace any existing handler.
     Incorrect handlers may cause system crashes!

Confirm? (type YES to confirm): YES

Result: SUCCESS
  Handler registered at vector 40
  New registration count: 46/192
```

**错误处理**：
```
[ERROR] Invalid vector: must be in range 32-223
[ERROR] Invalid handler address: null pointer
[ERROR] Vector already registered (use -f to override)
[ERROR] Operation cancelled by user
```

**实现要点**：
- **必须使用安全接口** `handler_register()`
- 验证向量范围（32-223）
- 验证处理程序地址非空
- 检查向量是否已被占用
- 严格的确认机制
- 记录注册历史

**安全考虑**：
- ⚠️ **高风险**：错误的处理程序会导致系统崩溃
- ⚠️ 必须在虚拟机中测试
- ✅ 使用安全接口，自动进行边界检查
- ❌ 禁止使用 `unsafe_handler_register_without_vecnum_check`（除非 `-f` 标志）

---

#### 4. `idtunreg <vector>` - IDT 处理程序注销

**功能描述**：
从指定中断向量注销处理程序。**危险操作**，可能导致中断丢失。

**数据来源/操作接口**：
```cpp
class x64_local_processor {
public:
    // 安全接口（推荐使用）
    bool handler_unregister(uint8_t vector);
    
    // 不安全接口（仅供高级用户使用）
    void unsafe_handler_unregister_without_vecnum_check(uint8_t vector);
};
```

**命令语法**：
```
idtunreg <vector> [-f]

参数：
  <vector>        - 中断向量号（32-223）
  [-f]            - 可选，强制注销，跳过确认提示

示例：
  idtunreg 40                # 注销向量 40
  idtunreg 0x28 -f           # 强制注销
```

**输出示例**：
```
=== IDT Handler Unregistration ===
⚠️  WARNING: This operation removes an interrupt handler!

Request:
  Processor:       Current (ID: 0)
  Vector:          40 (0x28)

Current Handler:
  Address:         0xFFFFFFFF80123456

⚠️  Unregistering will disable this interrupt!
     Ensure no device relies on this vector.

Confirm? (type YES to confirm): YES

Result: SUCCESS
  Handler unregistered from vector 40
  Remaining registrations: 44/192
```

**错误处理**：
```
[ERROR] Invalid vector: must be in range 32-223
[ERROR] Vector not registered (nothing to unregister)
[ERROR] Operation cancelled by user
```

**实现要点**：
- **必须使用安全接口** `handler_unregister()`
- 验证向量范围
- 检查向量是否已注册
- 显示当前处理程序地址
- 严格的确认机制

**安全考虑**：
- ⚠️ **高风险**：注销正在使用的中断会导致设备失效
- ⚠️ 必须先确认没有设备依赖该向量
- ✅ 使用安全接口

---

### Phase 2 - GS 槽操作 ⭐⭐⭐⭐

#### 5. `gsread <index>` - GS 槽读取

**功能描述**：
读取当前处理器 GS 槽中指定索引的值。GS 槽用于存储每处理器数据。

**数据来源/操作接口**：
```cpp
class x64_local_processor {
private:
    GS_struct gs_slot;  // uint64_t[64]
    constexpr static uint32_t GS_SLOT_MAX_ENTRY_COUNT = 0x40;
    
public:
    uint64_t GS_slot_get(uint32_t idx);
    // 超过索引返回 ~0，索引 0 被占用（静默失败）
};
```

**命令语法**：
```
gsread <index>

参数：
  <index>         - GS 槽索引（1-63，十进制或十六进制）

注意：索引 0 被系统占用，无法读取

示例：
  gsread 1                 # 读取索引 1
  gsread 0x10              # 读取索引 16
  gsread 63                # 读取最后一个可用索引
```

**输出示例**：
```
=== GS Slot Read ===

Request:
  Processor:       Current (ID: 0)
  Index:           1 (0x01)

Value:
  Raw:             0x00000000FFFFFFFF
  Decimal:         4294967295

GS Slot Info:
  Total Slots:     64
  Usable Range:    1-63 (slot 0 reserved)
  Max Index:       63 (0x3F)

Note: Index 0 is reserved for internal use.
```

**错误处理**：
```
[ERROR] Invalid index: must be in range 1-63
[ERROR] Index 0 is reserved and cannot be read
```

**实现要点**：
- 调用 `GS_slot_get(idx)`
- 验证索引范围（1-63）
- 索引 0 拒绝访问
- 显示人类可读的格式

**安全考虑**：
- ✅ 只读操作，非常安全
- ⚠️ 索引 0 被系统占用，访问会静默失败

---

#### 6. `gswrite <index> <value>` - GS 槽写入

**功能描述**：
写入当前处理器 GS 槽中指定索引的值。**危险操作**，可能破坏每处理器数据结构。

**数据来源/操作接口**：
```cpp
class x64_local_processor {
public:
    void GS_slot_write(uint32_t idx, uint64_t content);
    // 索引 0 静默失败，超过 63 静默失败
};
```

**命令语法**：
```
gswrite <index> <value> [-f]

参数：
  <index>         - GS 槽索引（1-63）
  <value>         - 要写入的值（十六进制或十进制）
  [-f]            - 可选，强制写入，跳过确认提示

示例：
  gswrite 1 0xFFFFFFFF           # 写入索引 1
  gswrite 0x10 12345             # 十进制值
  gswrite 63 0xDEADBEEF -f       # 强制写入
```

**输出示例**：
```
=== GS Slot Write ===
⚠️  WARNING: This operation modifies per-processor data!

Request:
  Processor:       Current (ID: 0)
  Index:           1 (0x01)
  Value:           0x00000000FFFFFFFF

Current Value:
  Old:             0x0000000000000000

⚠️  Writing to GS slot may affect kernel subsystems!
     Only proceed if you know what this slot is used for.

Confirm? (type YES to confirm): YES

Result: SUCCESS
  Value written to GS slot 1
```

**错误处理**：
```
[ERROR] Invalid index: must be in range 1-63
[ERROR] Index 0 is reserved and cannot be written
[ERROR] Operation cancelled by user
```

**实现要点**：
- 调用 `GS_slot_write(idx, value)`
- 验证索引范围（1-63）
- 索引 0 拒绝访问
- 显示当前值供对比
- 严格的确认机制

**安全考虑**：
- ⚠️ **中等风险**：可能破坏内核每处理器数据
- ⚠️ 必须了解 GS 槽的用途
- ⚠️ 建议在开发环境中测试

---

### Phase 3 - IOAPIC 管理 ⭐⭐⭐⭐

#### 7. `ioapicstat` - IOAPIC 状态查询

**功能描述**：
显示主 IOAPIC 的状态，包括 RTE（重定向表条目）数量、配置信息等。

**数据来源/操作接口**：
```cpp
class ioapic_driver {
private:
    uint8_t ioapic_id;
    uint8_t max_rte_num;
    
public:
    // 通过 main_router 全局指针访问
};

extern ioapic_driver *main_router;
extern spinlock_cpp_t interrupt_manage_lock;
```

**命令语法**：
```
ioapicstat

示例：
  ioapicstat             # 显示 IOAPIC 状态
```

**输出示例**：
```
=== IOAPIC Status ===

Controller Info:
  IOAPIC ID:         0
  Max RTEs:          24
  Base Address:      0xFEC00000

RTE Summary:
  Total Entries:     24
  Masked:            18
  Unmasked:          6
  Usage:             25.0%

Interrupt Routing:
  Configured IRQs:   6
  Available IRQs:    18

Note: Use 'ioapicirq' to see detailed IRQ mappings.
```

**实现要点**：
- 读取 `main_router->max_rte_num`
- 遍历所有 RTE，统计掩码状态
- 计算使用率
- 显示 IOAPIC ID 和基地址

---

#### 8. `ioapicirq <rte>` - IOAPIC IRQ 查询

**功能描述**：
查询指定 RTE（重定向表条目）的 IRQ 配置，包括向量、目标 APIC ID、触发模式等。

**数据来源/操作接口**：
```cpp
class ioapic_driver {
private:
    uint64_t get_rte_raw(uint8_t rte);
    
public:
    union RTE_remmap_union {
        uint64_t value;
        struct {
            uint64_t vector:8;
            uint64_t delivery_mode:3;
            uint64_t index_15:1;
            uint64_t delivery_status:1;
            uint64_t pin_polarity:1;
            uint64_t remote_irr:1;
            uint64_t trigger_mode:1;
            uint64_t mask:1;
            uint64_t reserved:31;
            uint64_t interrupt_format:1;
            uint64_t remmap_idx_0_14:15;
        } filed;
    };
    
    union RTE_compact_union {
        uint64_t value;
        struct {
            uint64_t vector:8;
            uint64_t delivery_mode:3;
            uint64_t destination_mode:1;
            uint64_t delivery_status:1;
            uint64_t pin_polarity:1;
            uint64_t remote_irr:1;
            uint64_t trigger_mode:1;
            uint64_t mask:1;
            uint64_t reserved:39;
            uint64_t destination:8;
        } filed;
    };
};
```

**命令语法**：
```
ioapicirq <rte> [format]

参数：
  <rte>           - RTE 索引（0-max_rte_num-1）
  [format]        - 可选，输出格式（默认 full）
                    full=完整解析, raw=原始值, compact=紧凑格式

示例：
  ioapicirq 0                # 查询 RTE 0
  ioapicirq 5 raw            # 原始值
  ioapicirq 10 compact       # 紧凑格式
```

**输出示例（完整解析）**：
```
=== IOAPIC IRQ Configuration (RTE 5) ===

Raw Value:
  0x0001002000000039

Bit Fields:
  [7:0]    Vector:            0x39 (57)
  [10:8]   Delivery Mode:     0 (Fixed)
  [11]     Destination Mode:  0 (Physical)
  [12]     Delivery Status:   0 (Idle)
  [13]     Pin Polarity:      0 (Active High)
  [14]     Remote IRR:        0 (Not asserted)
  [15]     Trigger Mode:      0 (Edge)
  [16]     Mask:              0 (Unmasked/Enabled)
  [55:17]  Reserved:          0
  [63:56]  Destination:       0x01 (APIC ID 1)

Configuration:
  IRQ Line:        5
  Target Vector:   57 (0x39)
  Target CPU:      APIC ID 1
  Delivery Mode:   Fixed
  Trigger:         Edge-triggered
  Polarity:        Active High
  Status:          ✓ Enabled (Unmasked)

Note: This IRQ is currently active and routing to CPU 1.
```

**输出示例（紧凑格式）**：
```
=== IOAPIC IRQ Summary (RTE 5) ===

RTE 5: Vector=57, Dest=APIC1, Mode=Fixed, Trigger=Edge, Polarity=High, Status=Enabled
```

**实现要点**：
- 调用 `get_rte_raw(rte)` 获取原始值
- 解析位字段
- 支持多种输出格式
- 显示人类可读的配置信息

---

#### 9. `ioapicreg <rte> <vector> <target>` - IOAPIC IRQ 注册

**功能描述**：
在指定 RTE 上注册 IRQ 路由配置。**危险操作**，可能影响硬件中断。

**数据来源/操作接口**：
```cpp
class ioapic_driver {
public:
    struct compact_flag {
        uint8_t vec;
        uint8_t target_apicid;
        uint8_t trigger_mode:1;
        uint8_t polarity:1;
    };
    
    KURD_t irq_regist(uint8_t rte, uint16_t remmap_idx, bool polarity);
    KURD_t irq_regist(uint8_t rte, compact_flag flag);
};

extern spinlock_cpp_t interrupt_manage_lock;
```

**命令语法**：
```
ioapicreg <rte> <vector> <target_apicid> [options] [-f]

参数：
  <rte>           - RTE 索引（0-max_rte_num-1）
  <vector>        - 目标中断向量（32-255）
  <target_apicid> - 目标 APIC ID
  [options]       - 可选，配置选项
                    edge=边沿触发(默认), level=电平触发
                    high=高电平有效(默认), low=低电平有效
  [-f]            - 可选，强制注册，跳过确认提示

示例：
  ioapicreg 5 57 1                   # 基本注册
  ioapicreg 5 57 1 level low         # 电平触发，低电平有效
  ioapicreg 5 57 1 edge high -f      # 强制注册
```

**输出示例**：
```
=== IOAPIC IRQ Registration ===
⚠️  WARNING: This operation modifies IOAPIC routing!

Request:
  RTE:             5
  Vector:          57 (0x39)
  Target APIC ID:  1
  Trigger Mode:    Edge
  Polarity:        Active High

Current Configuration:
  RTE 5:           Unconfigured

⚠️  Incorrect IRQ routing may cause hardware malfunctions!
     Ensure the vector is registered in IDT first.

Confirm? (type YES to confirm): YES

Result: SUCCESS
  IRQ registered on RTE 5
  Routing: IRQ 5 → Vector 57 → APIC ID 1
```

**错误处理**：
```
[ERROR] Invalid RTE index: out of range
[ERROR] Invalid vector: must be in range 32-255
[ERROR] Invalid APIC ID: target CPU not found
[ERROR] RTE already configured (use -f to override)
[ERROR] Operation cancelled by user
```

**实现要点**：
- 构建 `compact_flag` 结构
- 调用 `irq_regist(rte, flag)`
- 在全局锁 `interrupt_manage_lock` 保护下执行
- 验证参数合法性
- 严格的确认机制

**安全考虑**：
- ⚠️ **高风险**：错误的 IRQ 路由会导致硬件中断失效
- ⚠️ 必须确保目标向量已在 IDT 中注册
- ⚠️ 必须在虚拟机中测试
- ✅ 使用安全接口，自动进行参数验证

---

#### 10. `ioapicunreg <rte>` - IOAPIC IRQ 注销

**功能描述**：
从指定 RTE 注销 IRQ 路由配置。**危险操作**，可能导致硬件中断丢失。

**数据来源/操作接口**：
```cpp
class ioapic_driver {
public:
    KURD_t irq_unregist(uint8_t rte);
};
```

**命令语法**：
```
ioapicunreg <rte> [-f]

参数：
  <rte>           - RTE 索引
  [-f]            - 可选，强制注销，跳过确认提示

示例：
  ioapicunreg 5              # 注销 RTE 5
  ioapicunreg 5 -f           # 强制注销
```

**输出示例**：
```
=== IOAPIC IRQ Unregistration ===
⚠️  WARNING: This operation disables an IRQ route!

Request:
  RTE:             5

Current Configuration:
  Vector:          57
  Target APIC ID:  1
  Status:          Enabled

⚠️  Unregistering will disable this IRQ!
     Ensure no device relies on this RTE.

Confirm? (type YES to confirm): YES

Result: SUCCESS
  IRQ unregistered from RTE 5
  RTE is now masked/disabled
```

**错误处理**：
```
[ERROR] Invalid RTE index: out of range
[ERROR] RTE not configured (nothing to unregister)
[ERROR] Operation cancelled by user
```

**实现要点**：
- 调用 `irq_unregist(rte)`
- 在全局锁保护下执行
- 显示当前配置供确认
- 严格的确认机制

**安全考虑**：
- ⚠️ **高风险**：注销正在使用的 IRQ 会导致设备中断丢失
- ⚠️ 必须先确认没有设备依赖该 RTE

---

## 🔧 技术实现规范

### 1. IDT 向量验证

```cpp
bool validate_idt_vector(uint8_t vector) {
    // 0-31: CPU 异常，保留
    // 32-223: 可分配
    // 224-255: 保留
    return (vector >= 32 && vector <= 223);
}
```

### 2. GS 槽索引验证

```cpp
bool validate_gs_index(uint32_t idx) {
    // 0: 系统占用
    // 1-63: 可用
    return (idx >= 1 && idx < GS_SLOT_MAX_ENTRY_COUNT);
}
```

### 3. IOAPIC RTE 验证

```cpp
bool validate_ioapic_rte(uint8_t rte, uint8_t max_rte) {
    return (rte < max_rte);
}
```

### 4. 中断管理锁使用

```cpp
// 在修改中断配置时必须加锁
interrupt_manage_lock.lock();
// ... 执行 IOAPIC 操作 ...
interrupt_manage_lock.unlock();
```

---

## 📅 实施计划

### 第一阶段（Week 1-2）：处理器和 IDT
- [ ] 实现 `cpucount` 命令
- [ ] 实现 `idtstat` 命令
- [ ] 实现 `idtreg` 命令（安全接口）
- [ ] 实现 `idtunreg` 命令（安全接口）

### 第二阶段（Week 3-4）：GS 槽操作
- [ ] 实现 `gsread` 命令
- [ ] 实现 `gswrite` 命令
- [ ] 添加 GS 槽用途说明

### 第三阶段（Week 5-6）：IOAPIC 管理
- [ ] 实现 `ioapicstat` 命令
- [ ] 实现 `ioapicirq` 命令
- [ ] 实现 `ioapicreg` 命令（安全接口）
- [ ] 实现 `ioapicunreg` 命令（安全接口）

---

## ⚠️ 安全注意事项

### 高危操作清单
1. **`idtreg` / `idtunreg`** - IDT 修改（高风险）
2. **`gswrite`** - GS 槽写入（中等风险）
3. **`ioapicreg` / `ioapicunreg`** - IOAPIC 配置修改（高风险）

### 安全措施
1. ✅ 所有写操作必须有**确认机制**
2. ✅ 提供 `-f` 强制标志跳过确认
3. ✅ 显示**醒目的警告信息**
4. ✅ **参数严格验证**
5. ✅ 使用**安全接口**（禁止直接使用 unsafe 版本）
6. ✅ IOAPIC 操作必须在**全局锁**保护下执行

### 测试建议
1. 在**虚拟机**中测试所有命令
2. 先在**只读命令**上测试
3. 写操作先在**非关键向量/RTE**测试
4. 准备**恢复机制**（重启即可）

---

## 📝 待扩展功能

以下功能可能在后续版本中添加：

1. **IDT 批量操作**：`idtbatch` 批量注册/注销多个向量
2. **GS 槽批量读写**：`gsdump` 导出/导入 GS 槽内容
3. **IOAPIC 多控制器支持**：`ioapiclist` 列出所有 IOAPIC
4. **中断统计分析**：`intstat` 中断频率统计
5. **MSI/MSI-X 支持**：`msistat` PCI 设备 MSI 中断查询
6. **中断亲和性设置**：`irqaffinity` 设置中断目标 CPU

---

## 🔗 相关文档

- [内存诊断命令设计](./kshell_memory_commands_design.md)
- [内存操作命令设计](./kshell_memory_operations_design.md)
- [x86 架构绑定命令设计](./kshell_x86_architecture_commands_design.md)
- [i8042 键盘诊断命令设计](./kshell_i8042_keyboard_commands_design.md)

---

**文档版本**：v1.0  
**最后更新**：2026-04-26  
**作者**：Kernel Development Team
