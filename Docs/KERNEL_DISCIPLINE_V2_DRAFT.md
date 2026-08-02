# KERNEL_DISCIPLINE.md — 第十一章草案（Git Commit 史官纪律）

> 约束对象：AI 史官（Raven / OpenCode），非人类开发者
> 前提：AI 生成成本低但速度慢，代码库常处于"撕裂不过编译"状态
> 目标：原子性 · KISS · 粒度可控 — 不多不少

---

## 十一、Git Commit 史官纪律

### 11.1 一 AI 交互 · 一逻辑变更

AI 每次交互完成一次**独立的逻辑变更**，提交为一个 commit。

| 场景 | 拆 | 合 |
|------|----|----|
| 改了调度器 + NVMe 驱动两个子系统 | ✅ 拆成两个 commit | ❌ |
| 同一个接口签名改了 20 个文件 | ❌ | ✅ 合为一个 commit |
| 修复一个 bug + 顺便重构了附近逻辑 | ✅ 先 refactor commit，再 bugfix commit | ❌ |

**判断标准：** 这个 commit 的主题能用一句话说清楚？
- 能 → 粒度合适
- 要写两段才能说清 → 应该拆
- 一句话里出现了 "和/并且/同时" → 应该拆

### 11.2 拆改分离

> 重构不掺功能，功能不掺重构。

```
❌ 错误：在同一个 commit 里把函数从 a.cpp 挪到 b.cpp 又改了内部逻辑
✅ 正确：
  commit 1: move foo() from a.cpp to b.cpp (纯文件移动，不修改任何逻辑)
  commit 2: refactor foo() to use new allocator API (纯重构，不改行为)
  commit 3: add NUMA-aware allocation in foo() (纯功能新增)
```

每个 commit 的职责要么是 **"搬家不改逻辑"**，要么是 **"改逻辑不搬家"**，要么是 **"修 bug 不改结构"**。

### 11.3 无格式约束，有内容约束

AI 不是人在终端看 log，所以：

| 传统规则 | SparrowOS AI 纪律 |
|----------|------------------|
| 标题 ≤ 50 字符 | ❌ 不限制 |
| 正文 ≤ 72 列换行 | ❌ 不限制 |
| 标题首字母大写 | ❌ 不限制 |
| 标题不加句号 | ❌ 不限制 |
| ✅ 祈使语气 | 🟡 建议（不强求） |
| **正文写 what & why，不是 how** | **✅ 强制** |
| **引用关联 issue / task / bug** | **✅ 强制** |
| **Fixes: 引用被修复的 commit** | **✅ 强制（方便 bisect）** |

核心内容约束就三条：

```
1. 说清楚 what：这个 commit 改了什么东西
2. 说清楚 why：为什么这么改，动机是什么
3. 说清楚 ref：关联哪个 task / 修复了哪个 commit
```

代码本身证明 **how**，commit message 不重复写 how。

### 11.4 禁止 bisect-safe 承诺

> **本项目不保证每个 commit 都能编译通过。**
> 内核开发特性决定了代码经常处于"撕裂重构"状态。
> 但以下场景必须保持可编译：

| 必须编译通过 | 允许编译不过 |
|-------------|-------------|
| bugfix commit | 大型重构系列中的过渡 commit |
| 独立功能新增 | 拆改分离中的 "拆" 阶段 |
| 提交到 master/主干 | WIP / 实验分支 |

**判断标准：** 如果你在修一个 bug，改完后至少这个 bugfix commit 本身不能引入新的编译错误。
如果你在重构整个调度器，中间提交几个编译不过的过渡 commit 是可以接受的。

### 11.5 粒度把控：不多不少的基准

| 粒度 | 特征 | 处理 |
|------|------|------|
| ❌ 太碎 | 一个文件改个变量名就是一个 commit | 交互内积累，不要写完一行就 commit |
| ✅ 适中 | 一个子系统的一个逻辑变更 | 目标粒度 |
| ❌ 太粗 | 改了 5 个子系统，描述要写三大段 | 拆成系列 commit |

**实操指南：**

```
合适粒度的感觉：
- 打开 git log，只看标题就能知道每个 commit 在干什么
- git bisect 虽然不能保证每个 commit 可编译，但能定位到具体的逻辑变更
- revert 掉其中一个 commit，不会连带丢失其他不相关的改动

太细的信号：
- 标题只能写 "rename variable x to y" 而这种单行改法
- 连续三个 commit 的标题开头一模一样

太粗的信号：
- commit message 超过 30 行正文
- 标题写到一半发现必须写 "fix ... and refactor ..."
- 一个 commit 涉及两个以上不相关的子系统
```

### 11.6 KISS 原则在 commit 上的体现

```
✅ 保持简单的三条判断：
  1. 这个 commit 的意图是否单一且明确？
  2. 如果 reviewer 只看到标题，能不能猜出改了啥？
  3. 如果需要解释，一段话够不够？

❌ 复杂化的信号：
  1. commit message 需要画图/列表才能说清
  2. reviewer 需要看完全部代码才能理解意图
  3. 你自己写完三天后也看不懂这个 commit 在干嘛
```

### 11.7 AI 史官工作流

```
AI 交互开始
  ↓
理解任务意图
  ↓
判断：这个任务应该是一个 commit 还是系列 commit？
  ├─ 单一逻辑 → 一个 commit，直接干
  └─ 多逻辑 → 规划 commit 系列顺序，逐个执行
       ├─ commit 1: refactor (拆)
       ├─ commit 2: core logic change (核心改动)
       └─ commit 3: cleanup / docs (收尾)
```

**对应到 opencode.jsonc 的约束：**
- AI 不得在同一个交互中既改 A 又改 B 却只提交一个 commit
- AI 不得在一个 commit 里混入重构和功能
- AI 不得在 commit message 里只写 "update" 或 "fix" 这种无信息内容
