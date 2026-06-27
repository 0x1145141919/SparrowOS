# RavenFS 实现提纲

> 以磁盘布局为中心，映射三端实现的结构与顺序。
> 配套阅读: `disk_layout_v1.md`, `disk_struct.h`

## 三端概况

| 端 | 运行环境 | 语言 | 用途 | 缓存策略 |
|:---|:---------|:-----|:-----|:---------|
| **mkfs.ravenfs** | Linux userspace | C | 格式化分区 | 无需持久缓存，单次顺序操作 |
| **ravenfs.ko** | Linux kernel | C | 挂载读写 | 私有 buffer cache（不走 page cache） |
| **SparrowOS** | 自有 UEFI 内核 | C/C++ | 挂载读写 | 私有 buffer cache |

---

## 磁盘布局 —— 组件实现映射

### §1 Superblock (`disk_struct.h` 未收录, 见 `disk_layout_v1.md`)

| 操作 | mkfs | ravenfs.ko | SparrowOS |
|:-----|:-----|:-----------|:-----------|
| 初始化 | 计算各段范围 → 填充 struct → 写 block 0 | — | — |
| 读/校验 | — | 读 block 0 → 验证 magic/version → 校验 CRC | 同左 |
| 写/更新 | — | 只读（v1 不支持在线 resize 等需改 superblock 的操作） | 同左 |

### §2 Bitmap

| 操作 | mkfs | ravenfs.ko | SparrowOS |
|:-----|:-----|:-----------|:-----------|
| 初始化 | 所有元数据块预标记 = 1，其余 = 0 | — | — |
| alloc_block() | — | 扫描 bit → 标记 1 → 刷新 | 同左 |
| free_block() | — | 标记 0 → 刷新 | 同左 |
| 自管理 | 不参与，预标记 | 不参与自身块管理 | 同左 |

注意：block allocator 不是 B+tree 的一部分，是 bitmap 上的分配器，B+tree 操作时调它。

### §3 Inode Array

| 操作 | mkfs | ravenfs.ko | SparrowOS |
|:-----|:-----|:-----------|:-----------|
| 初始化 | 分配连续区域 → all zero → 写入根 inode | — | — |
| alloc_inode() | — | 扫描 inode array → 分配空槽 → 初始化 | 同左 |
| free_inode() | — | 清空 inode 槽 | 同左 |
| read_inode(n) | — | inode_array_base + n × 256 → 读盘 | 同左 |
| write_inode(n) | — | 脏标记 → 刷回 | 同左 |

inode array 是连续磁盘区域，三端的访问接口完全一致（block# = base + n×256/4096）。

### §4 B+tree 节点 (Bptree_node_t)

#### 4a. 节点 I/O 与缓存

这是**三端差异最大**的模块，原因是缓存层各不同。

| 操作 | mkfs | ravenfs.ko | SparrowOS |
|:-----|:-----|:-----------|:-----------|
| 读 | pread(block) → malloc'd buf | 私有 cache → miss → bio_alloc → submit_bio_wait | 自有 cache → miss → NVMe cmd |
| 写（刷回） | pwrite(block) → free | 私有 cache 标记 DIRTY → flush 时清零借用字段 → submit_bio | 同左 |
| 驱逐 | — | LRU → 脏则 flush → free | 同左 |
| 借用指针 | 不使用（用完即弃） | words[1] / entry[3..5].words[3] 缓存线性地址 | 同左 |

**重点：** 借用指针式缓存是 ravenfs.ko 和 SparrowOS 的特性，mkfs 不需要。

#### 4b. B+tree 算法（共享操作协议）

**操作协议**（不在代码中共享，按文档各端自己实现）：

| 操作 | 算法核心 |
|:-----|:---------|
| lookup(k) | 内部节点: target ≤ entry[i].key → child_i | 叶子: 二分/顺序找包含 k 的 extent |
| insert(key, val) | 找叶子 → 插入 → 如果溢出则半分裂 → 提 key 到父节点 → 递归 |
| delete(key) | 找叶子 → 删除 → 如果欠载则 borrow → 否则 merge → 递归 |
| split(node) | 取中点, 后半段到新节点, 提中点 key 到父 |
| merge(n1, n2) | n1 吸收 n2, 从父节点删掉 n2 的 key |
| range_scan(start, end) | 找 start → 沿 sibling 链表顺序扫到 end |

**关键约定**（各端一致，不依赖缓存实现）：
- 叶子 entry 用 `fblk_cmp` 严格保证 `[a, b)` 无重叠
- 内部节点 key = `max_R(child)`
- 分裂点取中点，向下取整或向上取整（需约定）
- sibling 链表只维护叶子层

### §5 Dentry

| 操作 | mkfs | ravenfs.ko | SparrowOS |
|:-----|:-----|:-----------|:-----------|
| mkdir | 创建 dir inode + 初始化 dentry btree 根 | — | — |
| creat | 创建 file inode + 插入 dentry | — | — |
| lookup(name) | — | hash dentry btree → 找名 → 返回 inode# | 同左 |
| unlink | — | 删 dentry | 同左 |

dentry 存在目录的 data B+tree 里，所以 B+tree 操作复用 §4b 的协议，只是 entry 的语义不同（key = hash, val = inode#）。

---

## 依赖顺序

```
                    ┌──────────────┐
                    │  disk_struct │
                    │  (三端共用)   │
                    └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │   Superblock  │
                    │    读 / 校验   │
                    └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │    Bitmap     │
                    │  alloc/free   │
                    └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │  Inode Array  │
                    │  alloc/free   │
                    └──────┬───────┘
                           ↓
             ┌─────────────┴─────────────┐
             ↓                            ↓
      ┌──────────────┐            ┌──────────────┐
      │ B+tree I/O + │            │  B+tree 算法  │
      │   node cache  │            │   (操作协议)   │
      └──────┬───────┘            └──────┬───────┘
             ↓                            ↓
             └─────────────┬─────────────┘
                           ↓
                    ┌──────────────┐
                    │  Inode 操作   │
                    │  data_root    │
                    │  三态语义     │
                    └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │   Dentry    │
                    │ (目录 B+tree)│
                    └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │  VFS 接入    │
                    │  (ko / OS)   │
                    └──────────────┘
```

---

## 各端起步建议

### mkfs.ravenfs（最轻量，可以最先做）

1. 直接 `pread` / `pwrite` 裸分区，无缓存
2. B+tree 只需顺序构建能力（不需要增删改），因为 mkfs 只需要写根 inode 和初始化目录
3. 可以仅实现 mkfs 自身的流程，不落 B+tree 的 insert/delete 全套
4. 用 `sfdisk` + `losetup` 就能测试

### ravenfs.ko（Linux 端）

1. 注册 `file_system_type` → `fill_super` → 读 superblock
2. 实现 `alloc_block` / `free_block`
3. 实现私有 buffer cache（参考 XFS xfs_buf：`kmalloc` + `bio_alloc` + `submit_bio_wait`）
4. 在私有缓存上实现 B+tree lookup → insert → split → delete → merge
5. 实现 inode 操作 + dentry → VFS 回调

### SparrowOS（自有内核端）

1. 复用 `disk_struct.h`，布局同 ravenfs.ko
2. 缓存走自有分配器 + 自有 NVMe 驱动
3. B+tree 算法逻辑同 ravenfs.ko（只是替换了 load_node / mark_dirty / submit_io 的实现）
4. VFS 层按 SparrowOS 自有接口实现

---

## 命名约定（建议）

```
src/include/ravenfs/       — 共用头文件（三端共享）
  disk_struct.h             ✓ 已有

Docs/ravenfs/              — 文档
  disk_layout_v1.md         ✓ 已有
  实现提纲.md                ← 本文

raid/
  mkfs.ravenfs/             — Linux userspace 格式化工具
  ravenfs.ko/               — Linux kernel module
  sparrowos/                — SparrowOS 端（可能内联进 kernel 镜像）
```

每端的源码用同一份 `disk_struct.h`。各端内部再根据缓存实现拆 `cache.c`、`btree.c`、`inode.c`、`dentry.c`。
