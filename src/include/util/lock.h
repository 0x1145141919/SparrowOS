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
class spinlock_cpp_t {
    uint8_t status;  
public:
    spinlock_cpp_t() : status(UNLOCKED){}
    
    void lock();
    bool is_locked();
    void unlock();
};
class reentrant_spinlock_cpp_t {
    
    u64ka complex;
    static constexpr uint64_t PID_SHIFT = 32;
    static constexpr uint64_t DEPTH_MASK = 0xFULL;
    //[0:3]为深度，超过0xF就panic,[31:63]为pid标记，用fast_get_processor_id获取
    //显然深度为0且pid为0时为未上锁
    public:
    reentrant_spinlock_cpp_t();
    void lock();
    void unlock();
    bool is_locked();
};
class reentrant_spinlock_guard {
    reentrant_spinlock_cpp_t& lock_ref;
public:
    explicit reentrant_spinlock_guard(reentrant_spinlock_cpp_t& lock);
    ~reentrant_spinlock_guard();
    reentrant_spinlock_guard(const reentrant_spinlock_guard&) = delete;
    reentrant_spinlock_guard& operator=(const reentrant_spinlock_guard&) = delete;
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
