#include "central_cache.hpp"
#include <algorithm>

namespace tcm {

Span* CentralCache::GetOneSpan(SpanList& list, size_t size) {
    // size 已是对齐后的定长规格（见 ThreadCache::Allocate 的 RoundUp）
    for (Span* it = list.Begin(); it != list.End(); it = it->_next) {
        if (!it->_freeList.Empty()) return it;
    }
    // 没有则向 PageCache 申请新 Span 并切块形成自由链表
    Span* span = PageCache::GetInstance()->NewSpan(SizeClass::NumMovePage(size));
    if (!span) return nullptr;                 // 系统内存耗尽：向上层逐级返回失败
    span->_objSize = size;
    char* start = static_cast<char*>(span->PageStart());
    char* end   = start + span->ByteSize();
    while (static_cast<size_t>(end - start) >= size) {
        span->_freeList.Push(start);
        start += size;
    }
    span->_useCount = 0;
    list.PushFront(span);
    return span;
}

size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size) {
    size_t index = SizeClass::Index(size);
    std::lock_guard<std::mutex> lk(_mtxList[index]);

    Span* span = GetOneSpan(_spanLists[index], size);
    if (!span) { start = end = nullptr; return 0; }   // 内存耗尽：返回 0，由上层转成失败
    size_t actual = std::min(batchNum, span->_freeList.Size());
    span->_freeList.PopRange(start, end, actual);
    span->_useCount += actual;
    return actual;
}

void CentralCache::ReleaseListToSpans(void* start, size_t size) {
    size_t index = SizeClass::Index(size);
    std::lock_guard<std::mutex> lk(_mtxList[index]);

    while (start) {
        void* next = *reinterpret_cast<void**>(start);
        Span* span = PageCache::GetInstance()->MapObjectToSpan(start);
        span->_freeList.Push(start);
        --span->_useCount;
        if (span->_useCount == 0) {
            // 所有块都归还，整块 Span 退回 PageCache
            _spanLists[index].Erase(span);
            span->_freeList.Clear();
            PageCache::GetInstance()->ReleaseSpanToPageCache(span);
        }
        start = next;
    }
}

} // namespace tcm