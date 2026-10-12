#pragma once
#include <algorithm>
#include "freelist.hpp"
#include "size_class.hpp"
#include "central_cache.hpp"

namespace tcm {

// 每线程缓存：static thread_local 保证每个线程一条自由链表数组，分配/释放天然无锁。
class ThreadCache {
public:
    static ThreadCache* Get() {
        static thread_local ThreadCache tc;
        return &tc;
    }

    void* Allocate(size_t size) {
        size = SizeClass::RoundUp(size);          // 归一化到定长规格，保证同一档所有块等长
        size_t index = SizeClass::Index(size);
        FreeList& list = _freeLists[index];
        if (!list.Empty()) return list.Pop();
        return FetchFromCentralCache(index, size);
    }

    void Deallocate(void* ptr, size_t size) {
        size = SizeClass::RoundUp(size);
        size_t index = SizeClass::Index(size);
        FreeList& list = _freeLists[index];
        list.Push(ptr);
        if (list.Size() >= list.MaxSize()) {
            ListTooLong(index, size);
        }
    }

    // 线程退出前把滞留块全部归还，避免误报泄漏
    ~ThreadCache() { Drain(); }

    void Drain() {
        for (size_t i = 0; i < SizeClass::NCLASS; ++i) {
            if (_freeLists[i].Empty()) continue;
            void* start = nullptr, * end = nullptr;
            _freeLists[i].PopRange(start, end, _freeLists[i].Size());
            CentralCache::GetInstance()->ReleaseListToSpans(start, SizeClass::Classes(i));
        }
    }

private:
    void* FetchFromCentralCache(size_t index, size_t size) {
        // 慢启动：随领取次数逐步放大批量数量，直到 NumMoveSize 上限
        size_t batchNum = std::min(_freeLists[index].MaxSize(), SizeClass::NumMoveSize(size));
        if (_freeLists[index].MaxSize() == batchNum) {
            _freeLists[index].SetMaxSize(batchNum + 1);
        }
        void* start = nullptr, * end = nullptr;
        size_t actual = CentralCache::GetInstance()->FetchRangeObj(start, end, batchNum, size);
        if (actual == 0) return nullptr;
        void* result = start;
        if (actual > 1) {
            void* rest = *reinterpret_cast<void**>(start);
            _freeLists[index].PushRange(rest, end, actual - 1);
        }
        return result;
    }

    void ListTooLong(size_t index, size_t size) {
        void* start = nullptr, * end = nullptr;
        _freeLists[index].PopRange(start, end, _freeLists[index].MaxSize());
        if (start) CentralCache::GetInstance()->ReleaseListToSpans(start, size);
    }

    FreeList _freeLists[SizeClass::NCLASS];
};

} // namespace tcm