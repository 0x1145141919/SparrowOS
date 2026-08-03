在init.elf的kernel_mmu模块由于init_bcb_juvenile的引入，可以把分散的页状态溶解到那个init_bcb_juvenile中，较之于之前有了完整的分配回收能力后，我想到了由于init.elf的kernel_mmu本质是给kernel.elf准备资产，不妨也做一个红黑树，但是是以字符串，也就是名字作为锚点，这样子更具有可维护性。同理，我又推广到这个整个传递范式，实际上整个init.elf交接给kernel.elf的以名字为锚点的一系列资产，也可以在init.elf/kernel.elf端各自的红黑树（虽然中间在那个交接区域应该是特化简洁的数据结构），这样子，init.elf的主要任务就是准备那个红黑树并且适时填表，kernel.elf的任务就是适时读取移走。当然，init.elf给kernel.elf还是需要准备其它隐状态的遗产。也就是说，init.elf的后phase 2的代码基本上统统重写

---

# 字符串锚点容器（String-Tag Asset Container）— 架构细化

> 状态：draft（设计讨论记录，随设计方决策可覆盖；落地以代码为准）
> 版本：2026-08-03 细化（基于设计方三问答复 + AI 方案讨论）

---

## 一、范式定位

传递协议演进三阶段：

| 阶段 | 形态 | 问题 |
|------|------|------|
| 一 | VM_ID 常量（`VM_ID_LOGBUFFER` 等） | 隐式编号，靠约定对应 |
| 二 | 一等字段（`init_to_kernel_header` 逐字段） | 太硬编码，新资产要动 header + info_fill + 各消费方 |
| 三（本范式） | **字符串锚点资产注册表** | 新资产 = 加一条 `asset_entry_t` |

核心主张：**以名字为锚点**。init.elf 与 kernel.elf 各自维护一棵 name→asset 红黑树，中间交接区用特化简洁的扁平结构（序列化 desc 数组）穿越。init.elf 的主要任务 = 准备那棵树并适时填表；kernel.elf 的任务 = 适时读取移走（并支持运行期 CRUD）。

---

## 二、资产条目（abi 层，三世界共用）

```cpp
struct asset_entry_t {   // abi/boot.h，init/kernel/transfer_pages 三态
    char* name;          // 多arg语法（见 §四）
    void* data;          // 解释依赖 name 的 arg1 → 路由表
};
```

`data` 三世界语义：

| 世界 | `data` 含义 | 访问方式 |
|------|-------------|----------|
| init.elf | 物理指针（UEFI 恒等映射） | 直接解引用 |
| transfer_pages（交接包） | **包基址相对偏移量** | `packet_va + offset` 重链 |
| kernel.elf | 内核虚拟地址 | 直接解引用 |

---

## 三、偏移 / 重定位约定（两级重链）

沿用 `boot.h` 既有信息包规范（`_offset` 相对包基址 `p`，可整包映射换基址）。

由于 `asset_entry_t.data` 指向的数据**物理上可能在包外**（log_buffer 实体、GS 复合体、hdstacks、kIMG 等），重定位分两级：

1. **一级重链（包内）**：`asset_entry_t` 数组、name 字符串、desc blob 全部放进包内 → `packet_va + offset` 即可访问。
2. **二级重链（包外资产）**：包内 desc blob 存 `{phys, va, size}` 等物理地址数值，kernel.elf 经 `Kspace_phyaddr_access_window` 把 phys 重链成内核 VA。

> 推论（已获设计方确认）：**包外资产一律经包内 desc blob 引用物理地址，不做转生拷贝**。

---

## 四、多arg name 语法 + 路由表

### 4.1 语法

`name` 是连续空格分隔的多arg 字符串（至少一个连续空格为分隔符）：

```
"<arg0> <arg1> <arg2...>"
  │      │      └─ 后续arg，由 arg1 类型自行定义
  │      └─ arg1：void* 解释类型 → 查路由表得 asset_kind
  └─ arg0：资产本名 → 红黑树键（锚点）
```

- 红黑树以 **arg0（本名）为键**，值存完整 name + data。
- 模块认领：`registry->read("log_buffer")` 用 arg0 查找。

### 4.2 路由表（纯数据字典，方案 A）

`src/include/abi/asset_route.h`（新建，两世界共用）：

```cpp
enum asset_kind : uint8_t {
    ASSET_KIND_MEM_INTERVAL = 0,   // data → vm_interval
    ASSET_KIND_MOVABLE_FILE = 1,   // data → movable_file_entry_t
    ASSET_KIND_PHYMEM_BLOB  = 2,   // data → 裸 blob（大小看后续 arg）
    ASSET_KIND_ARCH         = 3,   // 架构复合（GS/hdstacks 等）
    ASSET_KIND_SCALAR       = 4,   // data → uint64 标量
};

struct asset_route_entry_t {
    const char* type_name;    // 多arg name 的 arg1
    uint8_t     kind;
    uint16_t    desc_size;    // 该类型 desc blob 字节数（包内重定位/校验用）
};

inline constexpr asset_route_entry_t asset_route_table[] = {
    { "mem",     ASSET_KIND_MEM_INTERVAL, sizeof(vm_interval) },
    { "movable", ASSET_KIND_MOVABLE_FILE, sizeof(movable_file_entry_t) },
    { "blob",    ASSET_KIND_PHYMEM_BLOB,  0 },
    { "arch",    ASSET_KIND_ARCH,         0 },
    { "scalar",  ASSET_KIND_SCALAR,       sizeof(uint64_t) },
};
```

- `inline constexpr` 数组：编译期常量，零运行时构造（不违反"禁止全局 C++ 构造"纪律），跨 TU ODR 安全，落在 .rodata。
- 路由表只决定"解释类型"（kind / desc_size）；**具体解释逻辑留在各消费模块**按 kind 自处理——避免 abi 头反向依赖模块实现（方案 B 的函数指针解释器表被否决）。

---

## 五、双端双树结构

| 树 | 条目类型 | 职责 | 所在 |
|----|----------|------|------|
| kernel_mmu 树 | `kmmu_entry_t { vm_interval interval; char* property_name; uint64_t flags; }` | init 侧**映射台账**（给 kernel.elf 执行：kernel.elf 段 / GS / hdstacks / 窗口 / arch MMIO） | init.elf `kernel_mmu` |
| init 资产树 | `asset_entry_t` | init 侧**handoff 清单**（要传给 kernel 的全部命名资产） | init.elf（独立于 kernel_mmu） |
| kernel 注册表 | `asset_entry_t` | kernel 侧 CRUD 容器（boot 倒入 + 运行期增改删） | kernel.elf 全局单例 |

- **kernel_mmu 树与资产树相互独立**：kernel_mmu 管"我映射了什么"，资产树管"我要交给 kernel 什么"。二者可能覆盖同一物理资产（如 GS 复合体），但语义不同、互不依赖。
- kernel_mmu 树条目类型维持 `kmmu_entry_t` 即可（不随资产树变体）。

---

## 六、kernel 侧 CRUD 容器

```cpp
class resource_registry_t {          // kernel 侧全局单例
public:
    loc_code_t pour(const asset_entry_t* descs, uint64_t count);  // boot 一次倒入
    entry_t*   create(const char* name, void* data);              // 同名失败
    entry_t*   read  (const char* name);                          // O(log n)，按 arg0
    loc_code_t update(const char* name, void* data);              // 保留 key
    loc_code_t remove(const char* name);
    // 遍历（调试 dump / 模块扫全表）
private:
    Ktemplats::RBTree<asset_entry_t, name_cmp> m_tree;   // 键 = arg0
    spinlock_cpp_t lock;                                 // 多核安全
};
```

- 底层 `Ktemplats::RBTree` 原生支持 insert/find/erase；update = find 后原地改 value（节点存 T 值）。
- **boot 阶段只读认领**（init 定义的条目）；**运行期模块可 create/update/remove** 自己的资源条目（完整 CRUD）。

---

## 七、生命周期时序

```
init.elf:
  phase 2（内存准备，冻结不重构）
  phase 3a/3b：准备资产 → 填资产树（asset_entry_t）+ kernel_mmu 树（kmmu_entry_t）
  phase 4   ：序列化资产树 → 交接包（扁平 desc 数组 + name 串 + desc blob，包基址相对偏移）
  phase 4.5 ：跳转

kernel.elf:
  kernel_start → very_early_init:
    kpoolmemmgr_t::Init()            // 第一堆就绪（kinit.cpp:232）
    pour(desc 数组 → 注册表)          // ← 必须在信息包清零之前！（kinit.cpp:267 阅后即焚）
    bootstrap 资产认领（kIMG/pages_arr/FPA/GS/hdstacks/phymem_segments）
  ksetmem_8(transfer, 0, ...)        // 信息包阅后即焚
  mem_init / 各模块：registry->read() 认领 + 运行期 CRUD
```

> **铁约束**：`pour()` 只能在 `very_early_init` 内、`kernel_start` 清零信息包之前完成（kinit.cpp:267）。第一堆（kpoolmemmgr::Init）恰在 very_early_init 第一行，时序可行。

---

## 八、隐状态遗产（仍单独穿越，不进注册表）

- 结构性标量：`logical_processor_count`、`kIMG_self_size`、entry_vaddr、各 count。
- 大 blob：`phymem_segments`、`loaded_VM_intervals`。
- Init_v3 的 BCB desc 数组 + BCB_bitmaps 位图区。
- 这些继续走 `init_to_kernel_header` 的 offset 字段/独立段，注册表只管命名资产。

---

## 九、分阶段实施路线

| 阶段 | 范围 | 状态 |
|------|------|------|
| S1 | phase 2（2a-2e）：basic_allocator → init_bcb_juvenile 顶替分配，页状态溶解 | **冻结不重构**（现行） |
| S2 | abi 契约：`asset_entry_t` + 偏移/两级重链 + 多arg 语法 + `asset_route.h` 路由表 + header 最小变更 | 待开工 |
| S3 | init 侧重写：资产树 + kernel_mmu 树（kmmu_entry_t）+ phase_3a/3b 注册化 + info_fill 序列化 | 全重写 |
| S4 | kernel 侧：`resource_registry_t` CRUD + very_early_init pour + mem_init/模块认领迁移 | 全重写 |
| S5 | 溶解旧物：page_allocator 账本、VM_ID/一等字段删除、Init_v3 纯资产物理描述符并入注册表 | 收尾 |

> init.elf 后 phase 2 代码（phase_3a/3b/4/4.5 的 ctx 手记、info_fill 硬编码、page_allocator 账本）统统一并重写进本范式。

---

## 十、待定 / 遗留

1. 资产树在 init 侧是否必须为红黑树，还是扁平数组即可（树 = 可维护性/查找，非硬性）。
2. 各模块认领的先后与报错语义（未认领条目是否泄漏告警）。
3. 与 Init_v3 物理描述符（BCB desc + 位图）合流的具体条目命名。
