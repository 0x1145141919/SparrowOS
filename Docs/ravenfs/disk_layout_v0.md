# RavenFS — 磁盘布局设计（迭代中）

> 基于 2026-05-31 ~ 06-01 的讨论，后继改动时应更新本文件。
> 当前是设计师的设计备忘录，不是承诺书；槽点很多，但先写下来才能改。

---

## 1. 设计哲学

- **块组是磁盘的基本组织单元**，等长、连续排列
- 不存在独立的 HyperCluster 块——group 0 的 SuperCluster 同时也是全局超块
- 不存在 BGDT（Block Group Descriptor Table）——「第 k 组从 `k × clusters_per_group` 开始」
  这个计算是隐式的，不需要索引数组
- 随机容错：HyperCluster 备份散布在随机块组的 SuperCluster 尾部
- 全文 B+tree：数据块索引（per-file）、目录项（per-dir）、空闲块（全局）均使用 B+tree

---

## 2. 分区布局

```
LBA 0 ┌──────────────────────────────────────────────┐
      │ Protective MBR                               │
LBA 1 ├──────────────────────────────────────────────┤
      │ GPT Header                                   │
LBA 2 ├──────────────────────────────────────────────┤
      │ GPT Partition Entry Array                    │
      ├──────────────────────────────────────────────┤
      │ Partition: RavenFS                           │
      │                                             │
      │ BlockGroup[0]:                              │
      │   SuperCluster (也是 global HC primary)      │
      │   [cluster bitmap]                          │
      │   [inode bitmap]                            │
      │   [inode table]                             │
      │   [data clusters]                           │
      │                                             │
      │ BlockGroup[1]:                              │
      │   SuperCluster (可能内嵌 HC backup)          │
      │   [cluster bitmap]                          │
      │   [inode bitmap]                            │
      │   [inode table]                             │
      │   [data clusters]                           │
      │                                             │
      │ ...                                         │
      │                                             │
      │ BlockGroup[N-1]:                            │
      │   SuperCluster (可能内嵌 HC backup)          │
      │   ...                                       │
      │                                             │
      │ Partition: Swap                             │
      └──────────────────────────────────────────────┘
```

### 关键约束

```
每块组等长:
    group_k.base_cluster = k × clusters_per_group
    group_k 的范围 = [base, base + clusters_per_group)

clusters_per_group 在 v1 固定:
    单组大小 ≈ 32MB（= 8192 个 4K 簇）
    一个 2TB 分区 → ~65536 个块组
```

---

## 3. SuperCluster（1 个 cluster = 4096B）

group 0 的 SuperCluster 兼作全局超块（HyperCluster 的功能）。

```c
struct RavenSuperCluster {
    /* ══════════ 全局元数据（仅 group 0 完整，其余 group 的 is_hc_backup 判真时有备份）══════════ */
    uint64_t  magic;                  // RAVENFS_MAGIC = 0x5241564E "RAVN"
    uint32_t  version;                // 格式版本号

    uint32_t  cluster_size;           // 固定 4096
    uint64_t  total_clusters;         // 分区总簇数
    uint64_t  clusters_per_group;     // 每块组簇数

    uint32_t  root_inode;             // 根目录 inode 编号
    uint32_t  root_group;             // 根目录所在块组编号

    uint64_t  free_block_btree_root;  // 全局空闲块 B+tree 根簇号（0=未初始化）

    uint8_t   uuid[16];               // 分区 UUID
    uint8_t   volume_name[48];        // 卷标

    /* ══════════ HC 备份信息 ══════════ */
    uint8_t   is_hc_backup;           // 本组是否含有 HC 备份
    uint8_t   hc_backup_count;        // 全局 HC 备份总数
    uint16_t  hc_backup_groups[16];   // 备份所在的 group 编号列表（0=无效）

    /* ══════════ 本组自描述 ══════════ */
    uint32_t  group_index;            // 本组编号（必须是 k）

    // 以下均为组内偏移（以 簇 = 4096B 为单位）
    uint32_t  cluster_bitmap_off;     // 簇位图在本组内的起始偏移（簇）
    uint32_t  cluster_bitmap_count;   // 簇位图占用簇数
    uint32_t  inode_bitmap_off;       // inode 位图偏移
    uint32_t  inode_bitmap_count;     // inode 位图占用簇数
    uint32_t  inode_table_off;        // inode 表偏移
    uint32_t  inode_table_count;      // inode 表占用簇数

    /* ══════════ 校验 ══════════ */
    uint32_t  crc32;                  // 本 cluster CRC32（0 = 未校验）

    uint8_t   reserved[3980];         // → 精确填满 4096 - 4(crc32) = 4092
} __attribute__((packed));
```

### 挂载时怎么找到 root inode

```
1. group 0 的 SuperCluster 在 0 × clusters_per_group = cluster 0
2. 读它，CRC 校验
3. 校验失败 → 遍历 hc_backup_groups[] 读对应组的 SuperCluster
4. 从校验通过的 SuperCluster 拿到 root_group 和 root_inode
5. 计算 root_group 的 base_cluster，读该组 SuperCluster
6. SuperCluster → inode_table_off → 算出 root_inode 的精确位置
7. 读出 root inode
```

---

## 4. Inode（256B）

```c
#define RAVENFS_INODE_SIZE      256
#define RAVENFS_INLINE_MAX      128

struct RavenInode {
    uint32_t  uid;
    uint32_t  gid;
    uint32_t  parent_inode;           // 父目录 inode 编号
    uint32_t  parent_group;           // 父目录所在组

    uint64_t  file_size;              // 文件大小（字节）

    /* 文件标志 packed:
     *  bit 0    : 目录(1) / 普通文件(0)
     *  bit 1-3  : 用户权限 (rwx)
     *  bit 4-6  : 组权限
     *  bit 7-9  : 其他权限
     */
    uint16_t  flags;

    uint64_t  create_time;
    uint64_t  modify_time;
    uint64_t  access_time;

    /*
     * 数据块 B+tree 根簇号。
     * key   = 文件内逻辑簇索引 (0, 1, 2, ...)
     * value = 物理簇号
     *
     * 0 表示空文件或 inline data。
     */
    uint64_t  btree_root_cluster;

    /*
     * Inline data：当 file_size ≤ RAVENFS_INLINE_MAX 时，
     * 数据直接存这里，btree_root_cluster 必须为 0。
     */
    uint8_t   inline_data[RAVENFS_INLINE_MAX];

    uint32_t  link_count;
    uint32_t  generation;             // NFS generation
    uint8_t   pad[12];                // 凑满 256B
} __attribute__((packed));

static_assert(sizeof(RavenInode) == RAVENFS_INODE_SIZE);
```

### v1 → v2 的预期演进

```
v1 (now):     btree_root_cluster — 文件数据 B+tree
v1 possible:  btree_root_cluster + extra_extent_root — 大文件区分 data/extent tree
v2+:          0 号 btree_root 是 meta B+tree（类似 btrfs），
              从中可以查到 data tree / dir tree / attr tree 等
```

但 v1 先单一 btree_root 跑通。一切不必要的抽象在 v1 是负债。

---

## 5. B+tree 节点（1 cluster = 4096B）

```c
#define RAVENFS_BTREE_MAGIC    0x42545245  /* "BTRE" */

struct RavenBtreeHeader {
    uint32_t  magic;
    uint16_t  type;              // 0=internal, 1=leaf
    uint16_t  keys_count;        // 当前 key 数
    uint64_t  parent;            // 父节点簇号（0=root）
    uint64_t  prev;              // 前兄弟
    uint64_t  next;              // 后兄弟
    uint32_t  crc32;
    uint8_t   reserved[24];      // 充到 48B
} __attribute__((packed));

struct RavenBtreeEntry {
    uint64_t  key;
    uint64_t  value;
} __attribute__((packed));        // 16B

// 内部节点：header + entries[0..n]（最后 implicit child pointer 在 entry[0] 左侧）
// 叶子节点：header + entries[0..n]
```

B+tree 用途：

| 作用域 | key | value | 位置 |
|-------|-----|-------|------|
| 文件数据块 | 逻辑簇号 | 物理簇号 | per-file, inode.btree_root |
| 目录项 | hash(filename) | 见下文 | per-dir, inode.btree_root |
| 空闲簇分配 | 簇号 | 分配状态 flag | 全局, SuperCluster.free_block_btree_root |

### 目录项（存在目录文件的 B+tree 叶节点里）

```c
struct RavenDentry {
    uint64_t  hash;             // key = xxhash(filename)
    uint64_t  inode;            // inode 编号
    uint32_t  group;            // inode 所在块组
    uint8_t   name_len;         // 文件名长度
    uint8_t   file_type;        // DT_REG / DT_DIR
    uint16_t  _pad;
    char      name[];           // 变长
} __attribute__((packed));
```

hash 碰撞用链式解决：在同一个 leaf node 里连续存放相同 hash 的条目。

---

## 6. 块组内部布局（一个示例）

以 `clusters_per_group = 8192`（≈ 32MB）为例：

```
Group k 起始于 cluster = k × 8192
┌──────────┬──────────┬──────────┬──────────┬──────────────────┐
│ SC(1)    │ cls bmap │ ino bmap │ ino tb   │ data clusters    │
│          │ (varies) │ (varies) │ (varies) │ (剩下全归 data)  │
└──────────┴──────────┴──────────┴──────────┴──────────────────┘
  cluster 0  1~M        M+1~N      N+1~P     P+1 ~ 8191
             (≈1)       (≈1)       (≈16)     ≈ 8173 个 data cls

M = (clusters_per_group + 4096×8) / (4096×8)  ≈ 1
   // 簇位图: 每组 8192 个 bit = 1 个 cluster

N = M + (inodes_per_group + 4096×8) / (4096×8)
   // inode 位图: 每组 2048 个 bit ≈ 1 个 cluster

P = N + (inodes_per_group × 256) / 4096
   // inode 表: 2048 × 256 = 512KB = 128 clusters
```

---

## 7. 当前已知槽点（未解决）

| # | 槽点 | 现状 |
|---|------|------|
| 1 | 等长块组 → 磁盘末尾可能浪费 | 对于 2TB + 32MB 组 = 65536 组，尾组未使用空间 < 32MB，可接受 |
| 2 | 没有 BGDT → 遍历所有组找 backup？ | 不遍历。backup_groups[] 硬编码 HC 备份位置，最多 16 个 |
| 3 | 组 0 的 SC = HC primary → 组 0 坏了全局都炸 | 靠 backup_groups[] 中的其他组搭救 |
| 4 | is_hc_backup 所在的 SuperCluster 如果恰好也坏了？ | 概率低。backup_groups[] 每个都可独立校验，坏一个还有其他的 |
| 5 | clusters_per_group 固化后不好改 | v1 不做动态调整 |
| 6 | inode 固定大小 256B → 浪费 | 初期够用，v2 可以加 variable inode |
| 7 | backup_groups[16] 硬编码上限 | 16 个备份足够：开头 3 个 + 中间 5 个 + 末尾 5 个 + 随机 3 个 |

---

## 8. 与旧 initfs 的差异对照

| 项目 | 旧 initfs | RavenFS |
|------|-----------|---------|
| 超块 | HyperCluster @ cluster 0（独立块） | SuperCluster = group 0 的 SC，兼全局 |
| 块组索引 | BGDT 数组 @ cluster 1+ | 隐式数组：group_k 在 k × per_group |
| 块组描述 | BlocksgroupDescriptor（16B）+ SuperCluster | 只有 SuperCluster（自描述）|
| 数据指针 | file_index_table（12 dir + 3 level indirect）| btree_root_cluster（B+tree）|
| 目录搜索 | 线性扫 FileEntryinDir[] | 哈希 B+tree |
| 空闲管理 | bitmap（每组独立） | 全局 B+tree + bitmap 备选 |
| Inline data | 无 | 128B |
| 容错 | 主 HyperCluster 单份 | HC primary + 散布 backup |

---

## 9. 当前未定事项

- **空闲簇管理**：v1 用 bitmap（复用 initfs 现有代码）还是直接上 B+tree？
- **B+tree 实现**：C++ class 还是 C struct？SparrowOS 和 Linux 两端共享到什么程度
- **Inline data** 阈值：128B 是否合理
- **目录哈希**：xxhash 还是简单 DJB2（初期不需要防碰撞攻击）
- **SC 中的 `reserved[3980]`**：是否要在这预留给未来的 B+tree metadata root
