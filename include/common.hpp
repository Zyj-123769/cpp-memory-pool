#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

// 跨平台系统页内存分配：Windows 用 VirtualAlloc，Linux 用 mmap
#ifdef _WIN32
  #define NOMINMAX   // 避免 windows.h 的 min/max 宏污染 std::min/std::max
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
#endif

namespace tcm {

// ---- 基础常量 ----
static constexpr size_t PAGE_SHIFT = 13;            // 页大小 = 2^13 = 8KB
static constexpr size_t PAGE_SIZE   = size_t(1) << PAGE_SHIFT;
static constexpr size_t MAX_BYTES   = 256 * 1024;   // 小对象上限：256KB
static constexpr size_t MAX_PAGES   = 128;          // PageCache 单块最大页数

using PageID = uintptr_t;

// ---- 系统页内存申请 / 归还 ----
// 成功向系统申请内存的次数（供测试验证 Span 合并后未再触发 mmap/VirtualAlloc）
inline std::atomic<size_t>& SysAllocCalls() {
    static std::atomic<size_t> calls{0};
    return calls;
}

// 实际归还给系统的次数（供测试验证空闲 Span 归还 OS）
inline std::atomic<size_t>& SysFreeCalls() {
    static std::atomic<size_t> calls{0};
    return calls;
}

inline void* SystemAlloc(size_t npages) {
    size_t bytes = npages * PAGE_SIZE;
    void* p;
#ifdef _WIN32
    p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = nullptr;
#endif
    if (p) SysAllocCalls().fetch_add(1, std::memory_order_relaxed);
    return p;
}

inline void SystemFree(void* p, size_t npages) {
    if (!p) return;
#ifdef _WIN32
    (void)npages;   // VirtualFree 不需要页数
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, npages * PAGE_SIZE);
#endif
    SysFreeCalls().fetch_add(1, std::memory_order_relaxed);
}

// 按字节申请（自动对齐到页、内容清零），仅用于内部结构，生命周期为进程级、不做归还
inline void* SystemAllocBytes(size_t bytes) {
    size_t npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    return SystemAlloc(npages);
}

// ---- 地址 <-> 页号 ----
inline PageID PageIDFromAddr(void* p) {
    return reinterpret_cast<PageID>(p) >> PAGE_SHIFT;
}
inline void* AddrFromPageID(PageID id) {
    return reinterpret_cast<void*>(id << PAGE_SHIFT);
}

// ---- 永不析构的单例 ----
// placement new 到静态存储，避免：(1) 走 operator new 导致与重载后的全局 new 互相递归；
// (2) 主线程 ThreadCache 析构时访问已析构的单例。
template <typename T>
T* Immortal() {
    static typename std::aligned_storage<sizeof(T), alignof(T)>::type storage;
    static T* instance = new (&storage) T();
    return instance;
}

} // namespace tcm