#pragma once
#include <stdint.h>
#include "util/OS_utils.h"
static constexpr uint8_t LOCKED = 1;
static constexpr uint8_t UNLOCKED = 0;
struct lock_flags {
    uint8_t if_enable_accept_interrupt:1;
};
class interrupt_guard{
    uint8_t if_enable_accept_interrupt:1;
    public:
    interrupt_guard();
    ~interrupt_guard();
};
// enable_interrupt_guard — interrupt_guard 的对偶：
//   进入作用域即强制开中断（IF=1），离开时「恢复进入前的 IF 原状」。
// 用途：必须可被中断的核间 RPC 等待段——returnable_ipi_send / fly_ipi_send 在
//   等待对端回写 slot 期间不得关中断，否则与另一端互等即成死锁（IPI 只在本核
//   IF=1 时才被投递）。见 Docs/Memory/TLB-shootdown-v4-IPI-RPC规范.md。
// ⚠️ 不得在持有 IF=0 自旋锁（spinlock_interrupt_about_*_guard）的临界区内使用：
//   它会把中断开进锁里，破坏「持锁期间同核不重入」的前提。规范上 RPC 调用点
//   禁止持任何 IF=0 临界区，本 guard 正是该前提的落地手段。
class enable_interrupt_guard{
    uint8_t if_enable_accept_interrupt:1;   // 进入前的 IF，用于退出还原
    public:
    enable_interrupt_guard();
    ~enable_interrupt_guard();
};
// 读取当前 IF（v4 规范 R1/R3 硬断言的判据：内核 RPC 只允许线程态、
// 不持 IF=0 临界区——两者等价于入口 IF==1；中断门/IPI/#PF 均置 IF=0）。
bool local_irq_enabled();
class spinlock_cpp_t {
    uint8_t status;  
public:
    spinlock_cpp_t() : status(UNLOCKED){}
    
    void lock();
    bool is_locked();
    void unlock();
};
class spinlock_interrupt_about_guard {
    spinlock_cpp_t& lock_ref;
    lock_flags flag;
public:
    explicit spinlock_interrupt_about_guard(spinlock_cpp_t& lock);
    ~spinlock_interrupt_about_guard();
    spinlock_interrupt_about_guard(const spinlock_interrupt_about_guard&) = delete;
    spinlock_interrupt_about_guard& operator=(const spinlock_interrupt_about_guard&) = delete;
};
class spinrwlock_cpp_t{
    spinlock_cpp_t readlock;
    spinlock_cpp_t writelock;
    uint32_t readers=0;
public:
    spinrwlock_cpp_t()=default;
    void read_lock();
    void read_unlock();
    void write_lock();
    void write_unlock();
};
class spinrwlock_interrupt_about_read_guard {
    spinrwlock_cpp_t& lock_ref;
    lock_flags flag;
public:
    explicit spinrwlock_interrupt_about_read_guard(spinrwlock_cpp_t& lock);
    ~spinrwlock_interrupt_about_read_guard();
    spinrwlock_interrupt_about_read_guard(const spinrwlock_interrupt_about_read_guard&) = delete;
    spinrwlock_interrupt_about_read_guard& operator=(const spinrwlock_interrupt_about_read_guard&) = delete;
};
class spinrwlock_interrupt_about_write_guard {
    spinrwlock_cpp_t& lock_ref;
    lock_flags flag;
public:
    explicit spinrwlock_interrupt_about_write_guard(spinrwlock_cpp_t& lock);
    ~spinrwlock_interrupt_about_write_guard();
    spinrwlock_interrupt_about_write_guard(const spinrwlock_interrupt_about_write_guard&) = delete;
    spinrwlock_interrupt_about_write_guard& operator=(const spinrwlock_interrupt_about_write_guard&) = delete;
};
class trylock_cpp_t {
    uint8_t status;
    static constexpr uint8_t LOCKED = 1;
    static constexpr uint8_t UNLOCKED = 0;
    
public:
    trylock_cpp_t() : status(UNLOCKED) {}
    
    // 尝试获取锁，成功返回true，失败返回false
    bool try_lock();
    
    void unlock();
};
class spintrylock_cpp_t {
    uint8_t status;
    static constexpr uint8_t LOCKED = 1;
    static constexpr uint8_t UNLOCKED = 0;

public:
    spintrylock_cpp_t() : status(UNLOCKED) {}

    // 阻塞式获取：失败时自旋直到成功
    void lock();

    // 非阻塞尝试：失败立即返回false
    bool try_lock();

    void unlock();
};
class spintrylock_spin_guard{
    spintrylock_cpp_t& lock_ref;
    public:
    explicit spintrylock_spin_guard(spintrylock_cpp_t& lock);
    spintrylock_spin_guard(const spintrylock_spin_guard&) = delete;
    spintrylock_spin_guard&operator=(const spintrylock_spin_guard&)=delete;
    ~spintrylock_spin_guard();
};
class spintrylock_try_guard{
    spintrylock_cpp_t* lock_ref;
    public:
    explicit spintrylock_try_guard(spintrylock_cpp_t* lock);
    spintrylock_try_guard(const spintrylock_try_guard&) = delete;
    spintrylock_try_guard&operator=(const spintrylock_try_guard&)=delete;
    bool is_locked() const;
    ~spintrylock_try_guard();
};
