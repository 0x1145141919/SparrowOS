# RavenFS — Design Specification v0.1

## 1. 概述

RavenFS 是一个面向 NVMe SSD 的自制文件系统，同时运行在：
- **Linux 内核**（通过内核模块 ravenfs.ko，对接 Linux VFS）
- **SparrowOS**（移植 VFS 接口 + 同一套磁盘数据结构）

核心设计要求：
- 长期在 Linux 内核上运行，保证性能与可靠性
- B+ 树作为唯一索引结构（替代 ext 的间接块指针 + 线性目录项）
- GPT 分区表上层构建
- 两端访问同一块磁盘（通过 nvme_dev_switch.sh 切换）

---

## 2. 磁盘布局

### 2.1 GPT 分区

```
LBA 0     ┌─────────────────────────────┐
          │  Protective MBR             │
LBA 1     ├─────────────────────────────┤
          │  GPT Header                 │
LBA 2-33  ├─────────────────────────────┤
          │  GPT Partition Entry Array   │
          ├─────────────────────────────┤
          │  Partition 1: RavenFS       │  ~1.7T
          │  Partition 2: Swap          │  8-16G
          ├─────────────────────────────┤
          │  GPT Backup                 │
last LBA  └─────────────────────────────┘
```

分区表由 `parted` / `sgdisk` 手动创建，RavenFS 运行在某个 GPT 分区之上（不是裸盘）。

### 2.2 RavenFS 分区内部布局

```
Partition start  ┌──────────────────────────────────────┐
                  │  Superblock (primary, 1 block)      │
                  ├──────────────────────────────────────┤
                  │  Reserved blocks (故障备份区)        │
                  ├──────────────────────────────────────┤
                  │  Block group 0                       │
                  │    ├─ Block group descriptor table   │
                  │    ├─ Data block bitmap              │
                  │    ├─ Inode bitmap                   │
                  │    ├─ Inode table (若干 blocks)      │
                  │    └─ Data blocks                    │
                  ├──────────────────────────────────────┤
                  │  Block group 1...                    │
                  ├──────────────────────────────────────┤
                  │  ...                                 │
                  ├──────────────────────────────────────┤
                  │  Superblock (backup, 1 block)        │
Partition end    └──────────────────────────────────────┘
```

### 2.3 块大小

**固定 4096 bytes**（匹配 NVMe 的 4K 物理扇区，避免 RMW）。

---

## 3. 数据结构

### 3.1 Superblock (1 block = 4096B)

```c
#define RAVENFS_MAGIC  0x5241564E  /* "RAVN" */
#define RAVENFS_VERSION 1

struct ravenfs_superblock {
    uint32_t  magic;              // RAVENFS_MAGIC
    uint32_t  version;
    uint64_t  block_size;         // 4096
    uint64_t  blocks_total;       // 分区总块数
    uint64_t  blocks_free;        // 空闲块数
    uint64_t  inodes_total;       // inode 总数
    uint64_t  inodes_free;        // 空闲 inode 数

    // Block groups
    uint32_t  blocks_per_group;   // 每 group 块数
    uint32_t  inodes_per_group;
    uint32_t  bg_count;           // block group 数量

    // B+ tree root 们
    uint64_t  root_inode;         // 根目录 inode 编号
    uint64_t  inode_btree_root;   // inode 分配 B+ tree 根块号
    uint64_t  block_btree_root;   // 空闲块 B+ tree 根块号

    // 关键元数据偏移（块号）
    uint64_t  sb_block;           // 本 superblock 所在块号
    uint64_t  sb_backup;          // 备份 superblock 块号
    uint64_t  bgdt_block;         // Block Group Descriptor Table 起始块号

    uint8_t   uuid[16];           // 分区的 UUID
    uint8_t   volume_name[64];    // 卷名

    uint32_t  checksum;           // CRC32 of this superblock (0 时表示未校验)
    uint8_t   padding[3952 - 4];  // 填充至 4096 - 4 (checksum 已占)
    uint32_t  sb_checksum;        // 最后的 4 字节为校验和
} __attribute__((packed));
```

**为什么用 B+ tree 管理空闲块和 inode：**
- 替代 ext 的 bitmap，解决大分区下 bitmap 扫描性能退化
- B+ tree 的 key = 块编号 / inode 编号，value = 分配状态
- 支持高效的连续块分配（空间局部性）

### 3.2 Block Group Descriptor Table

```c
#define RAVENFS_BGDT_BLOCKS  1  // 暂定 1 block，可扩展

struct ravenfs_bg_desc {
    uint64_t  bg_block_bitmap;    // 块位图起始块号（备选，初期也保留）
    uint64_t  bg_inode_bitmap;    // inode 位图起始块号（备选）
    uint64_t  bg_inode_table;     // inode 表起始块号
    uint32_t  bg_free_blocks;     // 本 group 空闲块数
    uint32_t  bg_free_inodes;     // 本 group 空闲 inode 数
    uint32_t  bg_used_dirs;       // 本 group 目录数
    uint8_t   bg_flags;           // 预留标志位
    uint8_t   bg_pad[3];
} __attribute__((packed));
// 每个 desc 32B，一个 block 可容纳 128 个 block group
```

### 3.3 Inode (256 bytes)

```c
#define RAVENFS_INODE_SIZE  256
#define RAVENFS_BTREE_BLOCK_SIZE  4096  // B+ tree 节点同块大小

struct ravenfs_inode {
    uint16_t  i_mode;             // 文件类型 + 权限 (S_IFREG/S_IFDIR/S_IFBLK...)
    uint16_t  i_uid;
    uint32_t  i_gid;
    uint64_t  i_size;             // 文件大小（字节）
    uint64_t  i_atime;            // access time
    uint64_t  i_mtime;            // modify time
    uint64_t  i_ctime;            // change time
    uint32_t  i_links_count;      // 硬链接数
    uint32_t  i_flags;            // 属性标志

    // 数据块索引: B+ tree 根节点所在块号
    //   key   = logical block index within the file (0,1,2,...)
    //   value = physical block number on disk
    uint64_t  i_btree_root;       // B+ tree 根块号（0=空文件/尚未分配）

    // 小文件优化：inline data
    //   如果文件大小 ≤ 128 bytes，数据直接存这里，不分配 B+ tree
    uint8_t   i_inline_data[128]; // inline data（与 btree_root 互斥使用）

    uint32_t  i_generation;       // NFS 用 generation 号
    uint32_t  i_reserved;
    uint8_t   i_pad[64];
} __attribute__((packed));
```

**关键设计决策：**
- Inline data：≤128B 的文件不经过 B+ tree，数据直接嵌入 inode
- B+ tree 是文件级别还是分区级别？**文件级别** — 每个文件一个 B+ tree，以 `block_idx` 为 key

### 3.4 B+ Tree 节点 (1 block = 4096B)

```c
#define RAVENFS_BTREE_ORDER   64  // 每个节点最多 64 个 key（4K/64 ≈ 64）

struct ravenfs_btree_header {
    uint32_t  magic;              // 节点 magic: 0x42545245 "BTRE"
    uint16_t  type;               // 0=internal, 1=leaf
    uint16_t  num_keys;           // 当前 key 数
    uint64_t  parent;             // 父节点块号（0=root）
    uint64_t  prev_sibling;       // 前兄弟块号（叶节点用）
    uint64_t  next_sibling;       // 后兄弟块号（叶节点用）
    uint32_t  checksum;           // CRC32 of this block
} __attribute__((packed));

// __attribute__ ((__packed__)) of the struct is 36 bytes.
// Keys and pointers/values start at offset 36.

struct ravenfs_btree_entry {
    uint64_t  key;      // 逻辑块偏移 / 文件名 hash / inode 编号
    uint64_t  value;    // 物理块号 / inode 编号 / 其他
} __attribute__((packed));  // 16B per entry
```

每个节点可用空间 = 4096 - 36 = 4060 字节。B+ tree order（每个节点最大 key 数）计算：
- 内部节点：key 数 order，子节点指针 order+1
- 每个 entry 16B + 子指针 8B = 24B → 4060/24 ≈ 169 个 key
- 叶节点：每个 entry 16B → 4060/16 ≈ 253 个 key

但是为了简单和一致性，我们可以固定 order=128（足够大，也能容纳足够的 children指针）。

实际上，让我们简化：

```
Internal node:
┌────────┬────────┬────────┬───┬────────┐
│ header │ k1│p1  │ k2│p2  │   │ kn│pn  │
│        │(16+8=24B each) │   │        │
└────────┴────────┴────────┴───┴────────┘

Leaf node:
┌────────┬────────┬────────┬───┬────────┐
│ header │ k1│v1  │ k2│v2  │   │ kn│vn  │
│        │(16B each)       │   │        │
└────────┴────────┴────────┴───┴────────┘
```

**各种 B+ tree 用途：**

| B+ tree 角色 | 作用域 | key | value |
|---|---|---|---|
| 文件数据块索引 | 每个文件 | 逻辑块号 (LBN) | 物理块号 (PBN) |
| 目录项索引 | 每个目录 | 文件名 hash (uint64) | inode 编号 |
| 空闲 inode 管理 | 全局 | inode 编号 | 0/1 (free/used) |
| 空闲块管理 | 全局 | 块编号 | 块大小（碎片分配用）|

### 3.5 目录项

目录本身就是文件（inode），其数据块 B+ tree 存储的不是普通文件数据，而是目录项记录。每个 B+ tree 的 leaf entry 结构：

```c
struct ravenfs_dentry {
    uint64_t  hash;              // filename hash (key)
    uint64_t  inode;             // inode 编号
    uint8_t   name_len;          // 文件名长度
    uint8_t   file_type;         // 文件类型
    char      name[0];           // 变长文件名（紧跟在后面）
} __attribute__((packed));
```

**目录 B+ tree 组织：**
- Key = hash(filename)
- Value 里面编入 inode + name_len + file_type + name
- 因为 name 是变长的，一个 leaf entry 可能跨多个 entry 槽位
- Hash 碰撞用链式解决：相同 hash 的条目在同一 leaf node 的相邻槽位

**Hash 函数：** 使用 **xxhash**（快，64bit，分布均匀）。SparrowOS 也可以内置一个简单实现。

---

## 4. 关键算法

### 4.1 文件读流程

```
read(fd, buf, count)
  → VFS: ravenfs_file_read_iter()
    → 确定逻辑块范围 (offset..offset+count)
    → 遍历文件 B+ tree：
        key=逻辑块号 → value=物理块号
    → 对每个物理块: sb_bread(物理块号)
    → 拷贝到用户 buf
```

### 4.2 文件写流程

```
write(fd, buf, count)
  → VFS: ravenfs_file_write_iter()
    → 需要新块的逻辑块范围
    → 从空闲块 B+ tree 分配物理块
    → 插入/更新文件 B+ tree 条目
    → sb_bread + memcpy + sb_bwrite
```

### 4.3 目录查找

```
lookup(dir_inode, filename)
  → hash = xxhash(filename)
  → 在目录文件的 B+ tree 中搜索 key=hash
  → 匹配 leaf 中的具体文件名（解决 hash 碰撞）
  → 返回 inode 编号
```

### 4.4 空闲块分配

```
alloc_block()
  → B+ tree search(空闲块树, 0) 找到最小空闲块号
  → 标记为已用，更新树
  → 返回块号

alloc_contiguous_blocks(count)
  → B+ tree range scan(空闲块树, count) 找连续区间
  → 一次分配多个连续块（提高空间局部性）
```

---

## 5. Linux 内核模块架构

```
ravenfs.ko
├── ravenfs_super.c        # .fill_super, .kill_sb (super_operations)
├── ravenfs_inode.c        # iget, iput, read_inode, write_inode
├── ravenfs_dir.c          # dir inode_operations (lookup, create, link, unlink)
├── ravenfs_file.c         # file_operations (read_iter, write_iter, mmap)
├── ravenfs_btree.c        # B+ tree 核心（search, insert, delete, split, merge）
├── ravenfs_balloc.c       # 块分配器（封装空闲块 B+ tree）
├── ravenfs_ialloc.c       # inode 分配器
├── ravenfs_bread.c        # 块缓存/bio 封装
├── ravenfs_hash.c         # xxhash 实现
└── ravenfs.h              # 所有 on-disk 结构 + 内部函数声明
```

### 5.1 VFS 接口注册

```c
static struct file_system_type ravenfs_fs_type = {
    .owner    = THIS_MODULE,
    .name     = "ravenfs",
    .mount    = ravenfs_mount,
    .kill_sb  = ravenfs_kill_sb,
};

module_init(ravenfs_init);   // register_filesystem(&ravenfs_fs_type)
module_exit(ravenfs_exit);   // unregister_filesystem(&ravenfs_fs_type)
```

### 5.2 块 I/O 封装

```c
// 读/写一个 block（通过内核 bio 层）
struct buffer_head *ravenfs_sb_bread(struct super_block *sb, uint64_t block);
void ravenfs_sb_brelse(struct buffer_head *bh);
int ravenfs_sb_bwrite(struct buffer_head *bh);
```

使用内核的 `sb_bread()` / `sb_bwrite()` / `brelse()` 接口，基于 block_device 的 bio 层。

### 5.3 缓存策略

- **Buffer cache**: 内核自动维护 dirty page/buffer_head
- **Page cache**: `address_space_operations` 支持 mmap
- **不实现 journal**（v1）：依赖 fsck 恢复，简化第一期开发

---

## 6. 工具链

```
user_utils/
├── nvme_dev_switch.sh       # [已有] NVMe 模式切换
├── mkfs.ravenfs             # 格式化工具（创建 superblock + 空 B+ tree）
├── fsck.ravenfs             # 一致性检查（B+ tree 遍历 + 交叉验证）
└── btree_debug.c            # B+ tree 离线调试工具
```

### 6.1 mkfs.ravenfs 功能

```
mkfs.ravenfs [options] /dev/nvme1n1p1

选项:
  -b block_size      块大小（默认 4096）
  -i bytes_per_inode 每个 inode 覆盖字节数
  -L label           卷标
  -q                 静默模式
```

做的事情：
1. 写入 superblock
2. 初始化 Block Group Descriptor Table
3. 初始化空闲块 B+ tree（所有 block 标记为 free）
4. 初始化空闲 inode B+ tree
5. 创建根目录 inode（编号 2）
6. 在根目录 B+ tree 中创建 `.` 和 `..` 条目

---

## 7. 学习路径

这是让你系统掌握 Linux VFS 和块设备层的渐进路线：

### Phase 0 — 环境搭建（你已具备）
- ✅ 内核头文件 (`/lib/modules/$(uname -r)/build`)
- ✅ NVMe 裸设备 (nvme1n1，可 `sudo ./nvme_dev_switch.sh host` 切过来)
- ✅ device-mapper loop 设备用于测试 (`dd if=/dev/zero of=test.img bs=1M count=100`)

### Phase 1 — Hello World 内核模块
- 编译一个最小的 ko，打印 superblock 信息
- 熟悉 `make` 模块编译、`insmod` / `rmmod`、`dmesg`

### Phase 2 — B+ tree 实现
- 纯 C，无内核依赖，先用普通文件作为存储后端测试
- 完整的 search/insert/delete/split/merge
- 测试覆盖

### Phase 3 — mkfs.ravenfs
- 在 block device 上写入 superblock
- 初始化空的 B+ tree root 节点
- 用 `dd` 验证 block 内容

### Phase 4 — mount 到 Linux VFS
- 实现 `.fill_super` 解析 superblock
- 实现根目录的 `.lookup`
- 实现 readdir + read

### Phase 5 — 完整的文件操作
- create/unlink/rmdir
- write/truncate
- rename

### Phase 6 — 优化
- 连续块分配（空间局部性）
- 预读
- fsck 工具

---

## 8. 与 ext4 的关键差异

| 特性 | ext4 | RavenFS |
|------|------|---------|
| 数据块索引 | extents (B+ tree in ext4) / 间接块 | B+ tree（统一）|
| 目录结构 | hash tree (htree) / 线性 | B+ tree（统一）|
| 空闲空间 | bitmap (flex_bg) | B+ tree（统一）|
| inode 分配 | bitmap | B+ tree（统一）|
| Journal | jbd2（默认） | v1 无 journal |
| Inline data | 支持（inode + xattr） | 支持（inode 内 128B）|
| 块组 | flex_bg 分组 | 简化 block group |
| 扩展性 | 极大 | 初期 2TB 以内 |

核心哲学：**全文 B+ tree，消除 bitmap + 线性扫描**。代价是 B+ tree 的写入放大会略高，但对 NVMe 的随机写性能影响较小，换来的优势是：

- 恒定的 O(log n) 查找时间
- 无需 fsck 时的完整 bitmap 遍历
- 分级存储/压缩等高级特性天然可加
- 目录大文件的性能退化平滑（不像 ext 从 htree 退化为线性）

---

## 9. 优先实现顺序

```
Week 1:   B+ tree 核心（用户态验证）
Week 2:   mkfs.ravenfs 写入 superblock + 初始化树
Week 3:   内核模块骨架 + sb_bread 封装 + .fill_super
Week 4:   根目录 lookup + readdir → 可用 "ls" 看到内容
Week 5:   文件 read + write → 可用 "cat" / "cp"
Week 6:   create / delete / mkdir / rmdir / rename
Week 7:   小文件 inline data 优化
Week 8:   fsck 工具 + 连续块分配
```
