// 全局重载 operator new/delete，使 C++ 的 new/delete 表达式改走三级缓存分配器。
//
// 说明与安全边界：
//  * 内部结构（PageMap 节点、ObjectPool 切块）一律走 SystemAlloc（mmap/VirtualAlloc），
//    不经过 operator new；ThreadCache/PageCache 为 Immortal 单例，故不会与重载互相递归。
//  * 本文件只替换 C++ 的 new/delete 表达式（以及 new[]、nothrow、sized delete），
//    不拦截 C 的 malloc/free 符号（跨平台 CRT 符号差异大，且易与动态库内部堆冲突）。
//  * 若某指针并非来自本分配器（例如来自动态库内部堆），my_free 反查不到 Span 会安全空操作。
//  * 非对齐路径返回块最少 16B 对齐（最小规格为 16，页起始 8KB 对齐），满足 max_align_t。
//  * C++17 起新增带对齐的 operator new/delete 全家桶（见文件末尾）：对超过 max_align_t 的
//    过对齐请求，池内本就只保证 16B/整页对齐，故采用「多申请 + 对齐 + 前置指针槽」的方式满足，
//    释放时由对齐指针前的槽还原出原始指针再交回内存池。
#include <cstddef>
#include <cstdint>
#include <new>
#include "memory_pool.hpp"

void* operator new(std::size_t size) {
    void* p = tcm::my_malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t size) {
    void* p = tcm::my_malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return tcm::my_malloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return tcm::my_malloc(size);
}

void operator delete(void* p) noexcept { tcm::my_free(p); }
void operator delete[](void* p) noexcept { tcm::my_free(p); }

// sized delete：忽略 size，统一走无锁释放路径（nullptr 安全）
void operator delete(void* p, std::size_t) noexcept { tcm::my_free(p); }
void operator delete[](void* p, std::size_t) noexcept { tcm::my_free(p); }

void operator delete(void* p, const std::nothrow_t&) noexcept { tcm::my_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { tcm::my_free(p); }

// ==================== C++17 带对齐的 operator new/delete 全家桶 ====================
#if defined(__cpp_aligned_new) || __cplusplus >= 201703L
namespace {

// 池内最小对齐只有 16B（小对象）/整页（大对象），过对齐请求需自行对齐：
// 多申请 align + 指针槽，返回对齐地址，并在其前一个指针宽处保存原始地址。
inline void* AlignedAlloc(std::size_t size, std::size_t align) {
    // 防 size + align + sizeof(void*) 溢出
    if (size > SIZE_MAX - align - sizeof(void*)) return nullptr;
    void* raw = tcm::my_malloc(size + align + sizeof(void*));
    if (!raw) return nullptr;
    std::uintptr_t base    = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
    std::uintptr_t aligned = (base + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
    reinterpret_cast<void**>(aligned)[-1] = raw;   // 前置指针槽保存原始地址
    return reinterpret_cast<void*>(aligned);
}

inline void AlignedFree(void* p) {
    if (!p) return;
    tcm::my_free(reinterpret_cast<void**>(p)[-1]); // 由前置指针槽还原原始地址
}

} // namespace

void* operator new(std::size_t size, std::align_val_t al) {
    void* p = AlignedAlloc(size, static_cast<std::size_t>(al));
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size, std::align_val_t al) {
    void* p = AlignedAlloc(size, static_cast<std::size_t>(al));
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
    return AlignedAlloc(size, static_cast<std::size_t>(al));
}
void* operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
    return AlignedAlloc(size, static_cast<std::size_t>(al));
}

void operator delete(void* p, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { AlignedFree(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { AlignedFree(p); }

#endif // __cpp_aligned_new || C++17
