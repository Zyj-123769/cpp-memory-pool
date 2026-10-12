#include "page_cache.hpp"

namespace tcm {

void PageCache::_splitReturn(Span* span, size_t k) {
    if (span->_n > k) {
        Span* tail = _spanPool.New();
        tail->_pageId    = span->_pageId + k;
        tail->_n         = span->_n - k;
        tail->_chunkBase = span->_chunkBase;   // 尾块与头块同属一个 SystemAlloc 整块
        _registerSpan(tail);
        _pushFree(tail);
        span->_n = k;
    }
    span->_isUse = true;
    span->_useCount = 0;
    span->_objSize = 0;    // NewSpan 的约定：小对象由 CentralCache 赋值，大对象保持 0
    span->_freeList.Clear();
    _registerSpan(span);
}

Span* PageCache::NewSpan(size_t k) {
    if (k <= 0) k = 1;
    std::lock_guard<std::mutex> lk(_mtx);

    // 大对象（> MAX_PAGES 页）：直接向系统申请，只登记首页由地址反查；_objSize=0 标记大对象
    if (k > MAX_PAGES) {
        void* ptr = SystemAlloc(k);
        if (!ptr) return nullptr;
        Span* span = _spanPool.New();
        span->_pageId    = PageIDFromAddr(ptr);
        span->_n         = k;
        span->_chunkBase = span->_pageId;   // 大对象自成一个整块
        span->_objSize   = 0;
        span->_isUse     = true;
        _pageMap->set(span->_pageId, span);
        return span;
    }

    Span* span = nullptr;
    if (!_spanLists[k].Empty()) {
        span = _spanLists[k].PopFront();
        _freeBytes -= span->ByteSize();
    } else {
        for (size_t i = k + 1; i <= MAX_PAGES; ++i) {
            if (!_spanLists[i].Empty()) {
                span = _spanLists[i].PopFront();
                _freeBytes -= span->ByteSize();
                break;
            }
        }
    }
    if (!span) {
        void* ptr = SystemAlloc(MAX_PAGES);
        if (!ptr) return nullptr;
        span = _spanPool.New();
        span->_pageId    = PageIDFromAddr(ptr);
        span->_n         = MAX_PAGES;
        span->_chunkBase = span->_pageId;   // 整块起点
    }
    _splitReturn(span, k);
    return span;
}

void PageCache::ReleaseSpanToPageCache(Span* span) {
    if (!span) return;
    std::lock_guard<std::mutex> lk(_mtx);

    // 大对象：直接归还系统（先清空其占用的全部页映射）
    if (span->_n > MAX_PAGES) {
        PageID id = span->_pageId;
        size_t n  = span->_n;
        void* ptr = span->PageStart();
        for (size_t i = 0; i < n; ++i) _pageMap->set(id + i, nullptr);
        _spanPool.Delete(span);
        SystemFree(ptr, n);
        return;
    }

    span->_isUse = false;
    span->_useCount = 0;
    span->_objSize = 0;
    span->_freeList.Clear();
    _registerSpan(span);

    // 向前合并：仅合并「同一 SystemAlloc 整块内」的空闲相邻页，且合并后不超过单块页数上限。
    // 不跨块是归还 OS 的前提——保证任何合并结果的起止都落在同一次 SystemAlloc 的区间内。
    while (span->_pageId != span->_chunkBase && span->_n < MAX_PAGES) {
        Span* prev = _pageMap->get(span->_pageId - 1);
        if (!prev || prev->_isUse) break;
        if (prev->_chunkBase != span->_chunkBase) break;   // 不跨整块
        if (span->_n + prev->_n > MAX_PAGES) break;
        _eraseFree(prev);
        span->_pageId = prev->_pageId;
        span->_n += prev->_n;
        _spanPool.Delete(prev);
        _registerSpan(span);
    }
    // 向后合并（同样限制在同一整块内）
    while (span->_pageId + span->_n < span->_chunkBase + MAX_PAGES && span->_n < MAX_PAGES) {
        Span* next = _pageMap->get(span->_pageId + span->_n);
        if (!next || next->_isUse) break;
        if (next->_chunkBase != span->_chunkBase) break;
        if (span->_n + next->_n > MAX_PAGES) break;
        _eraseFree(next);
        span->_n += next->_n;
        _spanPool.Delete(next);
        _registerSpan(span);
    }

    _pushFree(span);
    _tryReleaseToOS();
}

void PageCache::_tryReleaseToOS() {
    // 只有「起点 == 整块起点 且 页数 == MAX_PAGES」的 Span 才是一次完整的 SystemAlloc，
    // 才能安全整块 munmap/VirtualFree；这类 Span 恰好落在 bucket[MAX_PAGES]。
    while (_freeBytes > _idleLimit && !_spanLists[MAX_PAGES].Empty()) {
        Span* victim = _spanLists[MAX_PAGES].Begin();
        if (victim->_pageId != victim->_chunkBase) break;   // 理论不会发生，防御性退出

        _eraseFree(victim);
        PageID id = victim->_pageId;
        size_t n  = victim->_n;
        void*  ptr = victim->PageStart();
        for (size_t i = 0; i < n; ++i) _pageMap->set(id + i, nullptr);
        _spanPool.Delete(victim);
        SystemFree(ptr, n);   // 真正把整块内存还给 OS
    }
}

} // namespace tcm
