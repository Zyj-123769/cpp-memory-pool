#pragma once
#include <mutex>
#include "span.hpp"
#include "size_class.hpp"
#include "page_cache.hpp"

namespace tcm {

// 居中缓存：每个 size class 一条 SpanList + 一把锁。
// ThreadCache 空槽时批量领取，满了批量归还；Span 计数归 0 才整块还给 PageCache。
class CentralCache {
public:
    static CentralCache* GetInstance() { return Immortal<CentralCache>(); }

    // 领取一批对象：返回实际数量，start..end 为有序链表
    size_t FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size);

    // 归还一段链表到所属 Span
    void   ReleaseListToSpans(void* start, size_t size);

private:
    Span* GetOneSpan(SpanList& list, size_t size);

    SpanList   _spanLists[SizeClass::NCLASS];   // 每规格一条 SpanList
    std::mutex _mtxList[SizeClass::NCLASS];     // 每规格一把锁
};

} // namespace tcm