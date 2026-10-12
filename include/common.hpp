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
    // Linux mmap 只保证 4KB 页对齐，但我们的「页」是 PAGE_SIZE（8KB）。
    // 若返回地址低 PAGE_SHIFT 位非 0，PageIDFromAddr 算出的页号会偏移，
    // 导致释放时 PageMap 反查不到正确 Span。这里多申请一页，对齐后把多余
    // 的前导页 munmap 掉，保证返回值严格 PAGE_SIZE 对齐。
    size_t alloc_bytes = bytes + PAGE_SIZE;
    void* raw = mmap(nullptr, alloc_bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return nullptr;
    uintptr_t addr = reinterpret_cast<uintptr_t>(raw);
    uintptr_t aligned = (addr + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    if (aligned != addr) {
        munmap(raw, aligned - addr);   // 释放前导 gap
    }
    // 尾部若有多余字节（对齐后总长度不足 alloc_bytes），释放之
    size_t used_end = aligned + bytes;
    uintptr_t raw_end = addr + alloc_bytes;
    if (used_end < raw_end) {
        munmap(reinterpret_cast<void*>(used_end), raw_end - used_end);
    }
    p = reinterpret_cast<void*>(aligned);
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
    // p 已保证 PAGE_SIZE 对齐（SystemAlloc 里处理过），直接 munmap 即可
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