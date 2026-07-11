# command_result_t 纪律规范

## 定位

`command_result_t` 是 NVMe 驱动模块的**内部命令返回通用语言**。它

- 不输出到模块外部（外部接口用 KURD_t）
- 统一表达三种结果语义：命令执行完成、超时、因内核子系统错误而未提交
- 与 KURD 五层模型协作：携带 KURD 在内核子系统之间透传

## 字段布局

```
128 bit union:
  cmd_spcify [63:0]   — 专用字段（语义由 result_type 决定）
  result_type [67:64]  — 结果类型枚举
  reserved   [111:68]  — 保留
  status     [126:112] — NVMe 命令完成状态（仅 command_executed 有效）
```

**不能通过 `dwords[4]` 直接读写字段**，必须通过 `fields` 结构体。

## 三种 result_type

### command_executed = 0

命令被成功提交到控制器且*已返回完成条目*。

| 字段 | 语义 |
|------|------|
| `cmd_spcify` | 命令完成条目的 `cmd_spcify` 原文 |
| `status` | NVMe 命令状态码（0 = 成功） |

消费端通过 `NVMe::status::is_error(r.fields.status)` 判断控制器是否报错。

### timeout = 1

命令提交到队列后，在超时周期内未收到完成条目。

`cmd_spcify` 和 `status` 未定义。

### not_success_kurd = 2

命令**未到达控制器**，因内核子系统（内存分配器、页表管理等）报错。

| 字段 | 语义 |
|------|------|
| `cmd_spcify` | `kurd_get_raw(kurd)` 对原始 KURD 的编码 |
| `status` | 未定义 |

---

## not_success_kurd 协议细则

### 生产端原则

必须使用 `NVMe_result_construtor_wrong_kurd(kurd)` 构造函数：

```cpp
static NVMe::command_result_t NVMe_result_construtor_wrong_kurd(KURD_t kurd)
{
    NVMe::command_result_t r;
    r.fields.result_type_t = NVMe::command_result_types::not_success_kurd;
    r.fields.cmd_spcify = kurd_get_raw(kurd);
    return r;
}
```

该函数位于 `io_queue_cmd.cpp:19`。

**禁止**：
```
// ❌ 丢失 KURD：cmd_spcify 默认为 0
return NVMe::command_result_t{ .fields = { .result_type = not_success_kurd } };
```

### 消费端原则

收到 `not_success_kurd` 时，必须解码 KURD 并向上透传：

```cpp
if (r.fields.result_type == NVMe::command_result_types::not_success_kurd) {
    return raw_analyze(r.fields.cmd_spcify);  // KURD 向上透传（约束二）
}
```

`raw_analyze(kurd_raw)` 是 `kurd_get_raw` 的逆操作，定义在 `os_error_definitions.h:79`。

### 桥接纪律（command_result_t → KURD_t）

任何返回 `KURD_t` 的函数，如果调用了返回 `command_result_t` 的子函数（如 `cmd_submit_and_process`、`io_queue_init`、`hmb_alloc`、`identify_*` 等），必须按以下优先级处理：

```
r.fields.result_type
├── not_success_kurd (2) → raw_analyze(r.fields.cmd_spcify) 向上透传
├── timeout (1)           → 构造 NVMe 位置级 KURD，填 timeout 语义
└── command_executed (0)
    ├── status 无错误      → 继续正常流程
    └── status 有错误      → 构造 NVMe 位置级 KURD，填 status 码
```

```cpp
// 标准桥接模式
NVMe::command_result_t r = some_nvme_command(...);
if (r.fields.result_type == NVMe::command_result_types::not_success_kurd) {
    return raw_analyze(r.fields.cmd_spcify);       // KURD 穿透
}
if (r.fields.result_type != NVMe::command_result_types::command_executed ||
    NVMe::status::is_error(r.fields.status)) {
    KURD_t err = nvme_default_error();
    err.event_code = EVENT_CODE_SOME_COMMAND;
    err.reason = ...;                               // 填 NVMe 语义原因
    return err;
}
```

---

## 当前违反点

以下生产站点使用 aggregate init 绕过 `cmd_spcify` 赋值，导致内存子系统的 KURD 丢失：

| 文件 | 行号 | 问题 |
|------|------|------|
| `NVMe_init_and_shutdown.cpp` | 98 | `hmb_alloc()` valloc 失败，KURD 未编码 |
| `NVMe_init_and_shutdown.cpp` | 107 | `hmb_alloc()` 地址翻译失败，KURD 未编码 |
| `io_queue_cmd.cpp` | 207 | `delete_io_sq()` drain timeout，KURD 未编码 |

修复方向：上述 site 应使用 `NVMe_result_construtor_wrong_kurd(kurd)` 替代 aggregate init。

---

## 角色边界

| 角色 | 职责 |
|------|------|
| 设计方 | 编排 `not_success_kurd` 协议、审核桥接代码 |
| AI | 根据本规范生成符合协议的生产/消费代码，不得在 `command_result_t` 协议外新增字段语义 |
