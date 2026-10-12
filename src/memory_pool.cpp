#include "memory_pool.hpp"
#include "common.hpp"
#include <cstdint>
#include <cstring>

namespace tcm {

void* my_malloc(size_t size) {
    if (size == 0) size = 1;
    if (size > MAX_BYTES) {
        // 大对象：直接按页向 PageCache 申请（Span._objSize == 0 标记为大对象）
        size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        Span* span = PageCache::GetInstance()->NewSpan(pages);
        if (!span) return nullptr;             // 系统内存耗尽
        return span->PageStart();
    }
    // 小对象：走线程缓存（内部会归一化到定长规格）
    return ThreadCache::Get()->Allocate(size);
}

void my_free(void* ptr) {
    if (!ptr) return;
    // 无锁反查所属 Span（PageMap 原子读），小对象释放快路径不抢任何全局锁
    Span* span = PageCache::GetInstance()->MapObjectToSpan(ptr);
    if (!span) return;
    size_t objSize = span->_objSize.load(std::memory_order_relaxed);
    if (objSize == 0) {
        // 大对象：走加锁慢路径归还
        PageCache::GetInstance()->ReleaseSpanToPageCache(span);
    } else {
        ThreadCache::Get()->Deallocate(ptr, objSize);
    }
}

size_t my_usable_size(void* ptr) {
    if (!ptr) return 0;
    Span* span = PageCache::GetInstance()->MapObjectToSpan(ptr);
    if (!span) return 0;                        // 非本分配器指针
    size_t objSize = span->_objSize.load(std::memory_order_relaxed);
    return objSize == 0 ? span->ByteSize() : objSize;   // 大对象：整块页大小
}

void* my_calloc(size_t num, size_t size) {
    if (size != 0 && num > SIZE_MAX / size) return nullptr;   // 防乘法溢出
    size_t total = num * size;
    void* p = my_malloc(total);                 // total==0 时内部按 1 处理，返回有效块
    if (p) std::memset(p, 0, total);
    return p;
}

void* my_realloc(void* ptr, size_t size) {
    if (!ptr)  return my_malloc(size);
    if (size == 0) { my_free(ptr); return nullptr; }   // 与 C 语义一致：realloc(p,0) 释放

    size_t old = my_usable_size(ptr);
    if (old == 0) return nullptr;               // 非本分配器指针，不动

    // 新规格若与旧规格同档，则原地复用，避免拷贝
    size_t newClass = size <= MAX_BYTES
                    ? SizeClass::RoundUp(size)
                    : ((size + PAGE_SIZE - 1) >> PAGE_SHIFT) << PAGE_SHIFT;
    if (newClass == old) return ptr;

    void* np = my_malloc(size);
    if (!np) return nullptr;                    // 分配失败保留原块，符合 realloc 语义
    std::memcpy(np, ptr, old < size ? old : size);
    my_free(ptr);
    return np;
}

} // namespace tcm