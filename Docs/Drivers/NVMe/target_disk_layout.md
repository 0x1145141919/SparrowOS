# 靶盘数据布局

## 物理设备

| 字段 | 值 |
|---|---|
| 设备路径 | `/dev/nvme1n1` |
| 型号 | WD Blue SN5000 2TB (15b7:5017) |
| 原生扇区大小 | 4096 字节 |
| 宿主 OS | Arch Linux, 通过 Linux NVMe 驱动写入 |

## 数据域范围

- **LBA 范围**: 0 ~ 65535（共 65536 个扇区）
- **总容量**: 65536 × 4096 = 268,435,456 字节（256 MiB）

此区域以外的 LBA 内容未定义，测试代码不得访问。

## 数据布局规则

将整个 LBA 空间视为连续的大端/小端无关的 `uint32_t` 数组。
每个扇区有 `sector_size / 4 = 1024` 个 `uint32_t` 元素。

**核心公式:**

```
对于 LBA 起始扇区 n，扇区内字节偏移 o (0 ≤ o < 4096)：
  uint32_t* data = (uint32_t*)sector_data;
  对 i 从 0 到 1023:
    data[i] = n × 1024 + i
```

### 示例

| LBA | u32[0] | u32[1] | u32[2] | ... | u32[1023] |
|---|---|---|---|---|---|
| 0 | 0 | 1 | 2 | ... | 1023 |
| 1 | 1024 | 1025 | 1026 | ... | 2047 |
| 2 | 2048 | 2049 | 2050 | ... | 3071 |
| ... | ... | ... | ... | ... | ... |
| 65535 | 67107840 | 67107841 | 67107842 | ... | 67108863 |

### 期望值验证函数

```c
// sector_size 由 NVMe Identify Namespace 解析得出（对本盘为 4096）
// buf 为扇区数据缓冲区，count 为连续 LBA 数量
bool verify_sequence(void* buf, uint64_t start_lba, uint32_t sector_size, uint32_t count) {
    uint32_t* p = (uint32_t*)buf;
    uint32_t u32_per_sector = sector_size / sizeof(uint32_t);  // 4096/4 = 1024
    uint32_t base = (uint32_t)(start_lba * u32_per_sector);
    uint32_t total_u32 = count * u32_per_sector;
    for (uint32_t i = 0; i < total_u32; i++) {
        if (p[i] != base + i)
            return false;  // 第一个不匹配即可判定失败
    }
    return true;
}
```

## 写入方式（历史记录）

靶盘由宿主 Linux 上的 `host_tools/nvme_write_target.c` 写入，
使用 `O_DIRECT | O_WRONLY` 打开 `/dev/nvme1n1`，
逐扇区 `pwrite` LBA 0 ~ 65535。

## 设计目的

在 PRP 测试中提供可预测的确定性回读校验，消除随机数据带来的歧义：

| PRP 分支 | 触发条件 |
|---|---|
| PRP1 only | 读 1 扇区 (4K) |
| PRP1 + PRP2 | 读 2 扇区 (8K) |
| PRP List (单页) | 读 ≥3 扇区 ≤ 66 扇区（单 PRP List 页可容纳 511 个 entry + 1 链指针） |
| PRP List (多页) | 读 ≥68 扇区，触发 daisy-chain |
