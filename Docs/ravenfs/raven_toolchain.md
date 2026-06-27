# RavenFS Toolchain

> 以磁盘布局为中心，映射全端实现的结构与顺序。
> 配套阅读: `disk_layout_v1.md`, `disk_struct.h`
>
> 所谓 toolchain 并非编译工具链——是让 RavenFS 接入各端 VFS 的**实现手段集合**。
> 与项目 CMake / 链接器等构建工具链无关。

## 全端概况

用户空间不是单点，而是三个独立的 build target，加上两个内核端构成五端。

| 端 | 运行环境 | 语言 | 用途 | 缓存策略 |
|:---|:---------|:-----|:-----|:---------|
| **mkfs.ravenfs** | Linux userspace | C | 格式化分区 | 无需持久缓存，单次顺序操作 |
| **expandravenfs** | Linux userspace | C | 离线扩容工具 (附只读诊断) | 调试状态可复现 1:1 |
| **ravendsrv** | Linux userspace | C | 用户态 VFS 小朝廷；直接读写分区镜像，假性"挂载"文件系统；内核端的开发前体 | 私有 buffer cache（同内核策略） |
| **ravenfs.ko** | Linux kernel | C | 挂载读写 | 私有 buffer cache（不走 page cache） |
| **SparrowOS** | 自有 UEFI 内核 | C/C++ | 挂载读写 | 私有 buffer cache |

### 用户空间三端的递进关系

```
mkfs.ravenfs       — 写盘布局，只是格式化
     ↓
expandravenfs      — 离线扩容（元数据搬迁：bitmap + inode array）
                    附只读诊断功能
     ↓
ravendsrv          — 完整读写，用户态开发内核态文件系统逻辑
                     = ko / SparrowOS 的前体和 runtime precursor
```

**ravendsrv** 是整个 toolchain 的核心环节：它在用户空间模拟了一个迷你 VFS，直接通过 `pread`/`pwrite` 读写分区镜像或设备文件。完整的 B+tree 算法、缓存策略、inode/dentry 操作全部在用户态调试通过后，再移植到两个内核端。这里不出错，内核端就只需要处理内核特有的接口（VFS 注册、BIO、锁模型），算法本身不需要再 debug。

---

## 磁盘布局 —— 组件实现映射

### §1 Superblock

| 操作 | mkfs | expandravenfs | ravendsrv | ravenfs.ko | SparrowOS |
|:-----|:-----|:------|:----------|:-----------|:-----------|
| 初始化 | 计算各段 → 填充 → 写 block 0 | — | — | — | — |
| 读/校验 | — | 读 → 展示字段 | 读 → 校验 | 读 → 校验 | 同左 |
| 快照 dump | — | 格式化打印 | 可选 | — | — |

### §2 Bitmap

| 操作 | mkfs | expandravenfs | ravendsrv | ravenfs.ko | SparrowOS |
|:-----|:-----|:------|:----------|:-----------|:-----------|
| 初始化 | 元数据块预标记=1，其余=0 | — | — | — | — |
| alloc_block | — | — | 扫描 → 标记 1 | 同左 | 同左 |
| free_block | — | — | 标记 0 | 同左 | 同左 |
| 诊断 | — | 展示已分配/空闲块分布 | 可选 | — | — |

### §3 Inode Array

| 操作 | mkfs | expandravenfs | ravendsrv | ravenfs.ko | SparrowOS |
|:-----|:-----|:------|:----------|:-----------|:-----------|
| 初始化 | 连续区域 → all zero → 根 inode | — | — | — | — |
| alloc_inode | — | — | 扫描空槽 → 初始化 | 同左 | 同左 |
| free_inode | — | — | 清空 | 同左 | 同左 |
| read_inode(n) | — | 读 → 展示 | base + n×256 → 读盘 | 同左 | 同左 |
| dump | — | — | 展示 inode 表 | — | — |

### §4 B+tree 节点 (Bptree_node_t)

#### 4a. 节点 I/O 与缓存

mkfs / expandravenfs 不做缓存（用完即弃），ravendsrv 和两个内核端各有一套。

| 操作 | mkfs | expandravenfs | ravendsrv | ravenfs.ko | SparrowOS |
|:-----|:-----|:------|:----------|:-----------|:-----------|
| 读 | pread → malloc | pread → malloc | pread → 私有 cache | bio_alloc → 私有 cache | NVMe cmd → 私有 cache |
| 写（刷回） | pwrite → free | 只读 | 标记 DIRTY → flush 时清零借用字段 → pwrite | 标记 DIRTY → flush → submit_bio | 同左 |
| 驱逐 | — | — | LRU → 脏则 flush → free | 同左 | 同左 |
| 借用指针 | 不使用 | 不使用 | 使用 | 使用 | 使用 |

**ravendsrv 的缓存策略跟内核端完全一致**——这是它作为前体的价值所在。

#### 4b. B+tree 算法（共享操作协议）

五端共享同一份操作协议。每端对着文档自己实现。

| 操作 | 算法核心 |
|:-----|:---------|
| lookup(k) | 内部节点: target ≤ entry[i].key → child_i | 叶子: 二分/顺序找 extent |
| insert(key, val) | 找叶子 → 插入 → 溢出则半分裂 → 提 key 到父 → 递归 |
| delete(key) | 找叶子 → 删除 → 欠载则 borrow → 否则 merge → 递归 |
| split(node) | 取中点, 后半段到新节点, 提中点 key 到父 |
| merge(n1, n2) | n1 吸收 n2, 从父删掉 n2 的 key |
| range_scan(start, end) | 找 start → 沿 sibling 链表顺序扫到 end |

**关键约定**（各端一致）：
- 叶子 entry 用 `fblk_cmp` 保证无重叠
- 内部节点 key = max_R(child)
- 分裂点取中点（精确取整方式需约定）
- sibling 链表只维护叶子层

### §5 Dentry

| 操作 | mkfs | expandravenfs | ravendsrv | ravenfs.ko | SparrowOS |
|:-----|:-----|:------|:----------|:-----------|:-----------|
| 创建目录 | 创建 dir inode + 初始化 dentry btree | — | 同左 | 同左 | 同左 |
| 创建文件 | 创建 file inode + 插入 dentry | — | 同左 | 同左 | 同左 |
| lookup(name) | — | hash dentry btree → 展示 | dentry btree 查找 | 同左 | 同左 |
| unlink | — | — | 删 dentry | 同左 | 同左 |
| dump dir | — | 列出目录内容 | 可选 | — | — |

---

## 依赖顺序

```
                    ┌──────────────┐
                    │  disk_struct │
                    │  (全端共用)   │
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
          ┌────────────────┼────────────────┐
          ↓                ↓                ↓
   ┌────────────┐   ┌────────────┐   ┌────────────┐
   │ ravendsrv  │   │ ravenfs.ko │   │ SparrowOS  │
   │ 用户态 VFS  │   │ Linux  VFS │   │ 自有 VFS   │
   └────────────┘   └────────────┘   └────────────┘
   ↑            ↑
   │            │
   │     ┌──────────┐
   │     │  expandravenfs   │
   │     │ (只读诊断)│
   │     └──────────┘
   │
   ┌──────────────┐
   │  mkfs.ravenfs │
   └──────────────┘
```

---

## 起步策略

**先做 mkfs → ravendsrv → expandravenfs → ko / SparrowOS**

### 1. mkfs.ravenfs（最轻量，最先做）

- `pread`/`pwrite` 裸分区或 loopback 文件
- 只写，不需要缓存
- B+tree 只需顺序构建能力（写根 inode + 初始化空目录）
- 不需要实现 insert/delete 全套
- 测试: `sfdisk` + `losetup`

### 2. ravendsrv（核心开发平台）

- 文件 + 分区直读模式的用户态进程
- **开发运行时**，不依赖内核模块
- 实现完整的 B+tree、私有缓存、inode/dentry 操作
- 借用指针式缓存在这里做验证
- 对外暴露什么接口？选择：
  - **方案 A**: 自定义 socket / unix domain socket 协议，裸文件操作（如 `open "hello.txt"` → 返回 fd）
  - **方案 B**: FUSE 挂载（`ravendsrv -o direct_io /mnt/raven`），走标准 VFS 路径，全端皆可用
  - **方案 C**: TLV 协议 + 本地端口，供 SparrowOS 开发时 mock 测试用

建议**先 A 再 B**：先裸 socket 跑通逻辑，再套 FUSE 层获得标准 POSIX 接口。

### 3. expandravenfs（扩容工具 + 只读诊断）

- 主功能: 离线扩容 — 将 bitmap 和 inode array 搬迁到扩容后的尾巴位置
- 附功能: 只读诊断 — 检查磁盘布局一致性，展示 superblock/inode/B+tree/bitmap 统计
- 用于调试 mkfs 输出和 ravendsrv 的修改是否破坏布局
- 类似 ext4 的 `resize2fs` + `debugfs` 二合一

### 4. ravenfs.ko（Linux 内核端）

- 注册 `file_system_type` → `fill_super` → 读 superblock
- 私有 buffer cache（`kmalloc` + `bio_alloc` + `submit_bio_wait`）
- B+tree 算法从 ravendsrv 移植（替换 I/O 层）
- VFS 回调接入

### 5. SparrowOS（自有内核端）

- `disk_struct.h` + 算法同 ravendsrv
- 缓存走自有分配器 + 自有 NVMe 驱动
- B+tree 算法逻辑同 ravendsrv（只是替换 load_node / mark_dirty / submit_io）
- VFS 层按 SparrowOS 自有接口实现

---

## 目录结构

```
src/include/ravenfs/
  disk_struct.h              ✓ 已有 — 全端共用

Docs/ravenfs/
  disk_layout_v1.md          ✓ 已有
  raven_toolchain.md         ← 本文

raid/                        — 将来可能迁移到独立仓库
  mkfs.ravenfs/               Linux userspace 格式化工具
  expandravenfs/                      Linux userspace 离线扩容 + 诊断工具
  ravendsrv/                  Linux userspace 守护进程（用户态 VFS）
  ravenfs.ko/                 Linux kernel module
  sparrowos/                  SparrowOS 端（或内联进 kernel 镜像）
```

每端实现内部建议模块划分：

```
    ├── main.c         入口 / CLI 解析
    ├── super.c        superblock 读/写/校验
    ├── bitmap.c       block allocator
    ├── inode.c        inode array + inode 操作
    ├── btree.c        B+tree 查找/插入/删除/分裂/合并
    ├── cache.c        私有 buffer cache（mkfs/expandravenfs 可无）
    ├── dentry.c       dentry B+tree 操作
    ├── vfs_ops.c      VFS 回调 / 对外接口（ko→file_operations, server→socket, OS→自有）
    └── disk_struct.h  软链接或头文件副本
```
