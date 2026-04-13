#include "util/lock.h"
#include "util/arch/x86-64/cpuid_intel.h"
static inline void cpu_relax()
{
#if defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
        __asm__ __volatile__("yield" ::: "memory");
#else
        __asm__ __volatile__("" ::: "memory");
#endif
}
static inline uint64_t read_rflags()
{
    uint64_t rflags;
    asm volatile("pushfq; popq %0" : "=r"(rflags) :: "memory");
    return rflags;
}
static inline bool get_if_enable_accept_interrupt(){
    
#if defined(__x86_64__) || defined(__i386__)
    return bool(read_rflags() & (1ull << 9));
#endif
}
static inline void disable_interrupts()
{
    asm volatile("cli" ::: "memory");
}
static inline void enable_interrupts()
{
    asm volatile("sti" ::: "memory");
}
static inline void restore_interrupts(const lock_flags& flags)
{
    if (flags.if_enable_accept_interrupt) {
        enable_interrupts();
    } else {
        disable_interrupts();
    }
}
void spinlock_cpp_t::lock()
{
    while (__atomic_test_and_set(&status, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&status, __ATOMIC_RELAXED) == LOCKED) {
            cpu_relax();
        }
    }
}

bool spinlock_cpp_t::is_locked()
{
    return __atomic_load_n(&status, __ATOMIC_RELAXED) == LOCKED;
}

void spinlock_cpp_t::unlock()
{
    __atomic_clear(&status, __ATOMIC_RELEASE);
}
void spinlock_interrupt_about_cpp_t::lock(lock_flags flag)
 {
        while (__atomic_test_and_set(&status, __ATOMIC_ACQUIRE)) {
                while (__atomic_load_n(&status, __ATOMIC_RELAXED) == LOCKED) {
                        cpu_relax();
                }
        }
        if(!flag.if_enable_accept_interrupt){
            disable_interrupts();
        }else enable_interrupts();
}

bool spinlock_interrupt_about_cpp_t::is_locked()
{
    return __atomic_load_n(&status, __ATOMIC_RELAXED) == LOCKED;
}
void spinlock_interrupt_about_cpp_t::unlock(lock_flags flag)
{
    __atomic_clear(&status, __ATOMIC_RELEASE);
    if(!flag.if_enable_accept_interrupt){
            disable_interrupts();
        }else enable_interrupts();
}
constexpr lock_flags DISABLE_INTERRUPT_FLAG=lock_flags{.if_enable_accept_interrupt=false};
spinlock_interrupt_about_guard::spinlock_interrupt_about_guard(spinlock_cpp_t& lock)
    : lock_ref(lock)
{
    lock_ref.lock();
    flag.if_enable_accept_interrupt=get_if_enable_accept_interrupt();
    #ifdef KERNEL_MODE
    disable_interrupts();
    #endif
}

spinlock_interrupt_about_guard::~spinlock_interrupt_about_guard()
{
    #ifdef KERNEL_MODE
    if(flag.if_enable_accept_interrupt){
        enable_interrupts();
    }else{
        disable_interrupts();
    }
    #endif
    lock_ref.unlock();
}

reentrant_spinlock_cpp_t::reentrant_spinlock_cpp_t()
{
    complex.store(0);
}

void reentrant_spinlock_cpp_t::lock()
{
    const uint64_t pid = static_cast<uint64_t>(fast_get_processor_id());

    while (true) {
        uint64_t cur = complex.load();
        uint64_t depth = cur & DEPTH_MASK;
        uint64_t owner = cur >> PID_SHIFT;

        if (depth == 0) {
            uint64_t desired = (pid << PID_SHIFT) | 1;
            if (complex.cmpxchg_strong(cur, desired)) {
                return;
            }
        } else if (owner == pid) {
            if (depth >= DEPTH_MASK) {
                __builtin_trap();
            }
            uint64_t desired = (cur & ~DEPTH_MASK) | (depth + 1);
            if (complex.cmpxchg_strong(cur, desired)) {
                return;
            }
        } else {
            cpu_relax();
        }
    }
}

void reentrant_spinlock_cpp_t::unlock()
{
    const uint64_t pid = static_cast<uint64_t>(fast_get_processor_id());
    while (true) {
        uint64_t cur = complex.load();
        uint64_t depth = cur & DEPTH_MASK;
        uint64_t owner = cur >> PID_SHIFT;

        if (depth == 0 || owner != pid) {
            return;
        }

        uint64_t desired;
        if (depth == 1) {
            desired = 0;
        } else {
            desired = (cur & ~DEPTH_MASK) | (depth - 1);
        }

        if (complex.cmpxchg_strong(cur, desired)) {
            return;
        }
        cpu_relax();
    }
}

bool reentrant_spinlock_cpp_t::is_locked()
{
    constexpr uint64_t DEPTH_MASK = 0xFULL;
    uint64_t cur = complex.load();
    return (cur & DEPTH_MASK) != 0;
}

reentrant_spinlock_guard::reentrant_spinlock_guard(reentrant_spinlock_cpp_t& lock)
    : lock_ref(lock)
{
    lock_ref.lock();
}

reentrant_spinlock_guard::~reentrant_spinlock_guard()
{
    lock_ref.unlock();
}

bool trylock_cpp_t::try_lock()
 {
        return !__atomic_test_and_set(&status, __ATOMIC_ACQUIRE);
}

    void trylock_cpp_t::unlock()
    {
        __atomic_clear(&status, __ATOMIC_RELEASE);
    }

void spintrylock_cpp_t::lock()
{
    while (__atomic_test_and_set(&status, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&status, __ATOMIC_RELAXED) == LOCKED) {
            cpu_relax();
        }
    }
}

bool spintrylock_cpp_t::try_lock()
{
    return !__atomic_test_and_set(&status, __ATOMIC_ACQUIRE);
}

void spintrylock_cpp_t::unlock()
{
    __atomic_clear(&status, __ATOMIC_RELEASE);
}
// 读锁实现
void spinrwlock_cpp_t::read_lock() {
    readlock.lock();      // 保存原始IF并关中断
    readers++;
    if (readers == 1) {
        // 第一个读者需要获取写锁
        writelock.lock();
    }
    readlock.unlock(); // 保持中断关闭
}

// 读解锁实现  
void spinrwlock_cpp_t::read_unlock() {

    readlock.lock();      // 保护readers计数器(不中断恢复)
    readers--;
    if (readers == 0) {
        // 最后一个读者释放写锁
        writelock.unlock();
    }
    readlock.unlock();
}

// 写锁实现
void spinrwlock_cpp_t::write_lock() {
    writelock.lock();     // 直接获取写锁
}

// 写解锁实现
void spinrwlock_cpp_t::write_unlock() {
    writelock.unlock();   // 直接释放写锁
}

// 读锁实现
void spinrwlock_interrupt_about_cpp_t::read_lock(lock_flags flag) {
    readlock.lock(DISABLE_INTERRUPT_FLAG);      // 保护readers计数器
    readers++;
    if (readers == 1) {
        // 第一个读者需要获取写锁
        writelock.lock(DISABLE_INTERRUPT_FLAG);
    }
    readlock.unlock(flag);
}

// 读解锁实现  
void spinrwlock_interrupt_about_cpp_t::read_unlock(lock_flags flag) {
    readlock.lock(DISABLE_INTERRUPT_FLAG);      // 保护readers计数器
    readers--;
    if (readers == 0) {
        // 最后一个读者释放写锁
        writelock.unlock(DISABLE_INTERRUPT_FLAG);
    }
    readlock.unlock(flag);
}

// 写锁实现
void spinrwlock_interrupt_about_cpp_t::write_lock(lock_flags flag) {
    writelock.lock(flag);     // 直接获取写锁
}

// 写解锁实现
void spinrwlock_interrupt_about_cpp_t::write_unlock(lock_flags flag) {
    writelock.unlock(flag);   // 直接释放写锁
}

spinrwlock_interrupt_about_read_guard::spinrwlock_interrupt_about_read_guard(spinrwlock_cpp_t& lock)
    : lock_ref(lock)
{
    lock_ref.read_lock();
    flag.if_enable_accept_interrupt=get_if_enable_accept_interrupt();
    disable_interrupts();
}

spinrwlock_interrupt_about_read_guard::~spinrwlock_interrupt_about_read_guard()
{
    if(flag.if_enable_accept_interrupt){
        enable_interrupts();
    }else disable_interrupts();
    lock_ref.read_unlock();
}

spinrwlock_interrupt_about_write_guard::spinrwlock_interrupt_about_write_guard(spinrwlock_cpp_t& lock)
    : lock_ref(lock)
{
    lock_ref.write_lock();
    flag.if_enable_accept_interrupt=get_if_enable_accept_interrupt();
    disable_interrupts();
}

spinrwlock_interrupt_about_write_guard::~spinrwlock_interrupt_about_write_guard()
{
    if(flag.if_enable_accept_interrupt){
        enable_interrupts();
    }else disable_interrupts();
    lock_ref.write_unlock();
}
interrupt_guard::interrupt_guard()
{
    this->if_enable_accept_interrupt=get_if_enable_accept_interrupt();
    disable_interrupts();
}
interrupt_guard::~interrupt_guard()
{
    if(if_enable_accept_interrupt){
        enable_interrupts();
    }else{
        disable_interrupts();
    }
}