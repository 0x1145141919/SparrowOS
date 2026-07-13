# 模块错误树 — Module Error Tree

## 概述

**模块错误树**是 Kernel Unified Result Descriptor (KURD) 的施工蓝图。它定义了

```
module_code → in_module_location → event_code → result → reason
```

的五层错误模型，确保 KURD 生成可追溯、可维护、不跨层混编。

- **module_code** 是 ABI 稳定层，也是模块错误树的根
- **domain** 在五层之外，是独立定性字段
- **level** 是独立的影响程度字段

---

## 五层模型

```
module_code              ← ABI 稳定层，树根
  └─ in_module_location   ← 模块内稳定位置 (树节点)
       └─ event_code      ← 该位置下的事件 (子节点)
            └─ result     ← 操作语义结果 (叶子)
                 └─ reason ← 具体原因 (子叶)
```

全局命名空间以 **module_code** 为根，即：模块命名空间。

**domain** 和 **level** 是 KURD 的独立字段，不在模块错误树内：

| 字段 | 职责 | 树内/树外 |
|------|------|-----------|
| `domain` | 语义域定性（core/arch/user/…） | 树外 |
| `module_code` | 顶层内核模块 | 树根 |
| `in_module_location` | 模块内稳定位置 | 树节点 |
| `event_code` | 该位置下的事件 | 子节点 |
| `result` | 操作语义结果 | 叶子 |
| `reason` | 具体原因 | result 的子叶 |
| `level` | 影响程度（info/error/fatal/…） | 树外 |

---

## 设计方职责（纪律）

模块错误树由内核开发者（设计方）手工编排，AI 负责根据它生成 KURD 代码。设计方需遵守以下纪律：

### 1. 层级纯净

每一层只依赖其父层，不跨层编码。

```
✅ 正确:
    reason = alloc_results::FAIL_REASONS::REASON_CODE_SIZE_IS_ZERO

❌ 错误（把事件信息编进了 reason）:
    reason = 0x42  // "alloc_size_zero" — 丢失了 event/location/module 信息链

❌ 错误（把位置信息编进了 reason）:
    reason = 0x100 // "kpoolmemmgr_alloc_fail" — location 本应由 in_module_location 承载

❌ 错误（把模块信息编进了 reason）:
    reason = 0x200 // "memory_alloc_fail" — module 本应由 module_code 承载
```

### 2. 位置稳定

`in_module_location` 必须是模块内**稳定编号**，不得绑定：

- 源码行号
- 函数地址
- 编译期变化的值（如 `__LINE__`）
- 版本号

允许的值：模块内巨型数据结构、子系统、阶段的稳定标识符。

### 3. Reason 空间契约

reason 命名空间 `[0x00, 0xFFF]` 在 `in_module_location` 层级下划分为两层。
**所有 location 统一使用以下分区，不按模块/位置调整：**

| 区间 | 层级 | 职责 |
|------|------|------|
| `[0x00, 0x100)` | `<location, result>` | 公共原因（256 个码位，跨事件共享） |
| `[0x100, 0xFFF]` | `<location, event, result>` | 事件私有原因 |

> 这种固定分区的优势：看到 raw KURD 的 reason 值域即知其归属层级。
> `0x00`~`0xFF` → 公共原因；`0x100`+ → 事件私有原因。

#### 3a. 公共原因 — `<in_module_location, result>` 层级

在同一 `in_module_location` 下，不同事件经常需要表达相同的失败原因
（如"地址校验失败"、"元数据损坏"、"参数为空"）。

这些公共原因定义在 **`<in_module_location, result>`** 层级，被该位置下所有产生该 result 的事件共享。

按 result 类型分组：

| 公共组 | 绑定 | 区间 |
|--------|------|------|
| `COMMON_FAIL_REASONS` | `<location, FAIL>` | `[0x00, 0x100)` |
| `COMMON_FATAL_REASONS` | `<location, FATAL>` | `[0x00, 0x100)` |
| `COMMON_RETRY_REASONS` | `<location, RETRY>` | `[0x00, 0x100)` |

> **每个 location 下必须存在 COMMON_FAIL_REASONS 和 COMMON_FATAL_REASONS，即使为空。**
> 空命名空间即表示该 location 下暂无非标公共原因，但保留了 `[0x00, 0x100)` 的扩展权。

#### 3b. 私有原因 — `<in_module_location, event_code, result>` 层级

当某个事件下的某个 result 有特定的、不被其他事件共享的原因时，
定义为该事件私有的原因码。

私有原因编码起点统一为 `0x100`，在该 `result` 对应的公共组上界之上。

```
私有起点 = 0x100（所有 location / result 类型统一）
```

#### 3c. 空间占用规则

> **所有 location 的公共原因共享同一区间 `[0x00, 0x100)`。**
> 私有原因统一从 `0x100` 起编，由设计方在 `<location, event_code, result>` 下自由编排。

```cpp
// ========== 示例：BuddyControlBlock location 下的原因空间 ==========

namespace FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES {

    // --- 公共原因 (shared across events, [0x00, 0x100)) ---

    // <LOCATION_CODE_BCB, FAIL> 公共原因 [0x00, 0x100)
    namespace COMMON_FAIL_REASONS {
        // 当前不需公共 FAIL 原因，空命名空间保留码位
    }

    // <LOCATION_CODE_BCB, FATAL> 公共原因 [0x00, 0x100)
    namespace COMMON_FATAL_REASONS {
        constexpr uint16_t BIN_TREE_CONSISTENCY_VIOLATION = 0x00;
    }

    // --- 事件私有原因 (event-specific, [0x100, 0xFFF]) ---
    // 私有原因编码从 0x100 起编

    // <LOCATION_CODE_BCB, EVENT_CODE_CONANICO_FREE, FAIL>
    namespace conanico_free_results::FAIL_REASONS {
        constexpr uint16_t FAIL_REASON_CODE_INVALID_ORDER       = 0x100;
        constexpr uint16_t FAIL_REASON_CODE_DOUBLE_FREE         = 0x101;
        constexpr uint16_t FAIL_REASON_CODE_INVALID_PAGE_INDEX  = 0x102;
    }

    // <LOCATION_CODE_BCB, EVENT_CODE_CONANICO_FREE, FATAL>
    namespace conanico_free_results::FATAL_REASONS {
        // BIN_TREE_CONSISTENCY_VIOLATION 由 COMMON_FATAL_REASONS 提供，不重复定义
    }
}
```

### 4. Free_to_use 不得逃逸

`free_to_use` 只用于模块内部局域语义，例如：

- 资源 ID
- 计数器/索引
- 分配 order
- 本模块调试上下文

**禁止**：
- 存放指针
- 跨 ABI 解释
- 作为公共状态传递

### 5. Event Code 约定

- `0` = 生命周期初始化事件
- `0xFF` = 生命周期结束事件
- 其余编码由设计方在 `<module_code, in_module_location>` 下自由编排

### 6. Result 与 Level 分离

| 字段 | 职责 |
|------|------|
| `result` | **语义**：成功( < 8 ) / 失败( >= 8 ) / 重试( 9 ) / 致命( 0xF ) |
| `level` | **影响程度**：info / notice / warning / error / fatal |

**标准 result 类型：**

| result 类型 | 值 | 语义 |
|------------|-----|------|
| `SUCCESS` | < 8 | 操作成功完成 |
| `FAIL` | >= 8 | 操作失败，可重试或上报 |
| `RETRY` | 9 | 操作可重试 |
| `FATAL` | 0xF | 内部状态损坏，不可继续 |
| `SUCCESS_BUT_SIDE_AFFECTS` | < 8 | 操作成功，但产生了附带副作用（如映射了低 16K 内存） |

`SUCCESS_BUT_SIDE_AFFECTS` 是 `SUCCESS` 的子类型——语义上成功，但调用方需检查副作用的 reason。

- `result` 直接决定调用方行为（继续/重试/停止）
- `level` 决定日志/告警/升级策略
- 两者独立：一个 FATAL result 未必是 FATAL level（可能是已知可恢复的致命错误）
- 一个 INFO level 的 result 也不意味成功（可能是低影响失败）

### 7. 命名规范 — UPPER_CASE

模块错误树中的所有常量名、命名空间名统一使用 **UPPER_CASE**（全大写+下划线），
避免与 C++ 成员函数/变量名（通常小写）碰撞。

| 成分 | 命名模式 | 示例 |
|------|----------|------|
| `in_module_location` 枚举值 | `UPPER_CASE` | `SCHEDULER`, `KTHREADS`, `BLOCK_QUEUE_SYSTEM` |
| 事件枚举值 | `EVENT_CODE_<NAME>` | `EVENT_CODE_INSERT_READY_TASK`, `EVENT_CODE_PUSH_TAIL` |
| 公共原因命名空间 | `COMMON_FAIL_REASONS`, `COMMON_FATAL_REASONS` | 固定这两个名字 |
| 事件私有原因命名空间 | `FAIL_REASONS`, `FATAL_REASONS`, `SUCCESS_REASONS` | 固定这三个名字 |
| 原因常量 | `UPPER_CASE` | `NULL_TASK_PTR`, `QUEUE_NOT_FOUND` |

**例外规则：**
- 结果分组命名空间（`insert_ready_task_results` 等）保持小写，因为它们不直接参与语义枚举，
  仅作为路径容器。

```cpp
// ✅ 正确
namespace SCHEDULER_EVENTS {
    namespace COMMON_FAIL_REASONS {
        constexpr uint16_t NULL_TASK_PTR = 0x00;
    }
    constexpr uint8_t EVENT_CODE_INSERT_READY_TASK = 1;
    namespace insert_ready_task_results {  // 小写：路径容器
        // ...
    }
}

// ❌ 错误：event_code 用的小写，与成员函数同名
constexpr uint8_t insert_ready_task = 1;  // 与 insert_ready_task() 函数冲突
```

### 8. 进制约定

所有 reason 常量值使用 **十六进制**（`0x...`）：

```cpp
// ✅ 正确
constexpr uint16_t REASON_CODE_BAD_PARAM    = 0x00;
constexpr uint16_t REASON_CODE_ADDR_INVALID = 0x01;
constexpr uint16_t REASON_CODE_NO_MEM       = 0x10;

// ❌ 错误（十进制）
constexpr uint16_t REASON_CODE_BAD_PARAM    = 0;
constexpr uint16_t REASON_CODE_NO_MEM       = 16;
```

十六进制的优势：reason 值域(`0x00`-`0xFF` 公共 / `0x100`+ 私有)一目了然。

---

## KURD 模块化制造范式（AI 生成规范）

以下为 AI 生成 KURD 相关代码时的行为规范，严格遵循模块错误树。

### 范式 1：命名空间按树展开

命名空间根 = module_code 对应的模块命名空间。在该空间内逐层嵌套：

```cpp
// ========== Memory 模块错误树 ==========

// 树根：module_code = MEMORY
namespace MEMMODULE_LOCATIONS {

    // 第一层：in_module_location
    constexpr uint8_t LOCATION_CODE_FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK = 32;

    // 第二层：事件层 + 原因空间
    namespace FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES {

        // --- 公共原因（<location, result> 层级）---
        namespace COMMON_FAIL_REASONS { }
        namespace COMMON_FATAL_REASONS {
            constexpr uint16_t BIN_TREE_CONSISTENCY_VIOLATION = 0x00;
        }

        // --- 事件（<location, event> 层级）---
        constexpr uint8_t EVENT_CODE_CONANICO_FREE = 2;

        // --- 叶子（<location, event, result> 层级下的私有原因）---
        namespace conanico_free_results::FAIL_REASONS {
            constexpr uint16_t FAIL_REASON_CODE_INVALID_ORDER = 0x100;  // 从 0x100 起编
            constexpr uint16_t FAIL_REASON_CODE_DOUBLE_FREE   = 0x101;
        }
    }
}
```

### 范式 2：KURD 三阶段构造流程

每个 `<in_module_location>` 下的函数使用**三阶段构造法**：

```
阶段一：模板   → 预填 domain, module_code, in_module_location, result, level
阶段二：注入   → 填入 event_code
阶段三：填充   → 在结果叶子处填入 reason
```

#### 阶段一：位置级模板函数

每个 `in_module_location` 提供一组模板函数，预填该位置固定的字段：

```cpp
// <LOCATION_CODE_BCB> 下的模板函数
KURD_t default_kurd()
{
    return KURD_t(0, 0,
        module_code::MEMORY,
        MEMMODULE_LOCATIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK,
        0, 0,
        err_domain::CORE_MODULE);
}

KURD_t default_success()
{
    KURD_t k = default_kurd();
    k.result = result_code::SUCCESS;
    k.level  = level_code::INFO;
    return k;
}

KURD_t default_error()
{
    KURD_t k = default_kurd();
    k = set_result_fail_and_error_level(k);  // result=FAIL + level=ERROR
    return k;
}

KURD_t default_fatal()
{
    KURD_t k = default_kurd();
    k = set_fatal_result_level(k);           // result=FATAL + level=FATAL
    return k;
}
```

#### 阶段二：事件函数内注入 event_code

```cpp
KURD_t some_event_function(...)
{
    // 阶段一：取模板
    KURD_t success = default_success();
    KURD_t error   = default_error();
    KURD_t fatal   = default_fatal();

    // 阶段二：注入 event_code（每个要用的模板都要注入）
    success.event_code = EVENT_CODE_SOME_EVENT;
    error.event_code   = EVENT_CODE_SOME_EVENT;
    fatal.event_code   = EVENT_CODE_SOME_EVENT;

    // ... 函数逻辑 ...
```

#### 阶段三：结果叶子处填充 reason

```cpp
    // 成功路径
    if (condition_ok) return success;

    // 失败路径 — 填充 reason 后返回
    if (bad_addr) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_BAD_ADDR;
        return error;
    }

    // 致命路径
    if (metadata_broken) {
        fatal.reason = COMMON_FATAL_REASONS::REASON_CODE_METADATA_DESTROYED;
        return fatal;
    }
}
```

完整的形式：

```cpp
KURD_t free_buddy_way(phyaddr_t base, uint64_t size)
{
    using namespace MEMMODULE_LOCATIONS::
        FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::
        FREE_RESULTS;

    // 阶段一：取模板
    KURD_t success = default_success();
    KURD_t error   = default_error();

    // 阶段二：注入 event
    error.event_code   = EVENT_CODE_FREE;
    success.event_code = EVENT_CODE_FREE;

    // 阶段三：逻辑 + 在叶子处填 reason
    if (!is_addr_belong(base) || !is_addr_belong(base + size - 1)) {
        error.reason = FAIL_REASONS::FAIL_REASON_CODE_BASE_NOT_BELONG;
        return error;
    }
    // ... 释放逻辑 ...
    return success;
}
```

**不省略、不猜测、不合并。** 每个返回路径都必须携带完整的七字段 KURD。

### 范式 3：使用 using 声明简化原因引用

在事件函数的开头使用 `using namespace` 引入原因命名空间，避免全路径冗余：

```cpp
KURD_t some_event_function(...)
{
    using namespace MEMMODULE_LOCATIONS::
        FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::
        FREE_RESULTS;

    KURD_t success = default_success();
    KURD_t error   = default_error();
    success.event_code = EVENT_CODE_FREE;
    error.event_code   = EVENT_CODE_FREE;

    if (!addr_belong()) {
        error.reason = FAIL_REASONS::FAIL_REASON_CODE_BASE_NOT_BELONG;
        return error;
    }
    return success;
}
```

### 范式 4：原因选择优先公共原因

当某个原因被同一 `in_module_location` 下多个事件共享时，优先使用公共原因组：

```cpp
// ✅ 正确：free 和 clear 共用同一个公共 FATAL 原因
fatal.reason = COMMON_FATAL_REASONS::BIN_TREE_CONSISTENCY_VIOLATION;
return fatal;

// ❌ 错误：在 private 组中重复定义已被公共组覆盖的原因
namespace conanico_free_results::FATAL_REASONS {
    // 重复！COMMON_FATAL_REASONS 已定义
    constexpr uint16_t BIN_TREE_CONSISTENCY_VIOLATION = 0x00;
}
```

### 范式 5：不得语义复用膨胀

当需要新的原因码时，即使其字面含义与某个已存在的 reason 值相同，
也不得直接借用——应在新的事件路径下分配新的 reason 值。
语义绑定的不是 reason 数值，而是它在模块错误树中的路径。

---

## 协作流程

```
[设计方] 定义/更新模块错误树（ModuleErrorTree.md + 头文件枚举）
         • 编排 in_module_location
         • 编排各 location 下的公共原因和事件私有原因
       ↓
[AI]    根据模块错误树生成 KURD 构造代码
         • 每个 in_module_location 提供 default_kurd/success/error/fatal 模板
         • 事件函数内：取模板 → 注入 event_code → 叶子填 reason
         • 公共原因用公共组，不复制到事件私空间
       ↓
  完整填写：
    tree: module_code, in_module_location, event_code, result, reason
    extra: domain, level
       ↓
  KURD → 语义完备、路径可追溯
```

## AI 行为约束

### 约束一：模块错误树必须经审核再落地

AI 生成的模块错误树（范式一：命名空间和枚举定义）**必须提交给设计方审核**，
审核通过后方可落地到实际头文件和 KURD 构造代码中。

AI 不得未经审核即修改或新增下列内容：

- `in_module_location` 枚举值
- 事件枚举值
- 公共原因 / 私有原因枚举的定义及数值编排
- 模块错误树的层级结构

审核方式：AI 将生成的模块错误树以 diff 形式呈报，设计方确认后合并。

### 约束二：传递原则 — 原始 KURD 向上透传

KURD 携带了完整的五层错误树路径（module_code → in_module_location → event_code → result → reason），
其设计目标之一就是**实现跨模块的原始错误信息传递**。

当一个接口调用了产生 KURD 的子接口，且决定在此处终结 EVENT（不再重试或下沉处理）：

- **必须将子接口返回的原始 KURD 向上一级传递**
- 不得拦截、翻译、包裹或替换子接口的 KURD
- 只有这样，上层调用者才能通过 KURD 的五层路径追溯到最内层的失败根因

```cpp
// ✅ 正确：原始 KURD 向上透传
KURD_t inner_kurd;
phyaddr_t pbase = FreePagesAllocator::alloc(size, params, state, inner_kurd);
if (!success_all_kurd(inner_kurd)) {
    return inner_kurd;  // 完整保留子接口的 module→location→event→result→reason 链
}

// ❌ 错误：拦截、翻译或替换原始 KURD
KURD_t inner_kurd;
phyaddr_t pbase = FreePagesAllocator::alloc(size, params, state, inner_kurd);
if (!success_all_kurd(inner_kurd)) {
    error.reason = COMMON_FAIL_REASONS::REASON_CODE_ALLOC_FAILED;
    return error;  // 丢失了分配器内部的具体失败原因
}
```

这条原则保证：**不管调用栈多深，失败的根因 KURD 始终可达，跨模块可追溯**。

> KURD 的 domain, module_code, in_module_location, event_code 等字段本身就是全局 ABI 稳定标识，
> 上层调用者完全可以根据这些字段在自己的模块错误树中解释错误——不需要中间层翻译。

### 约束三：业务代码中的 KURD 用空占位，不碰模块错误树

当命令仅涉及**修改业务逻辑代码**，而 KURD 相关字段作为附带的错误返回时：

- **不允许** AI 自行创建或修改模块错误树定义
- 应使用**空的 KURD 占位**（构造一个临时 KURD_t，字段不填或填 0）
- 待后续配合模块错误树统一编排

```cpp
// ✅ 正确：业务代码改造时，KURD 用空占位，不碰模块错误树
KURD_t k;  // 空 KURD 占位
k.result = result_code::FAIL;
return k;

// ❌ 错误：在业务代码修改中擅自新增枚举定义
namespace MY_NEW_EVENTS {
    constexpr uint8_t EVENT_CODE_FOO = 1;  // 不允许！需经审核
}
```

## 角色边界

| 角色 | 职责 | 禁止事项 |
|------|------|----------|
| 设计方 | 编排模块错误树的拓扑和语义枚举 | — |
| AI | 根据已审核的错误树生成 KURD 构造代码 | 未经审核不得编辑错误树；业务代码中不得自建错误树 |

双方共同遵守：**不跨层编码，不省略字段，不复用未归属的语义**。
