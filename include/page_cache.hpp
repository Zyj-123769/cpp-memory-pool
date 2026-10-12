#pragma once
#include <mutex>
#include "span.hpp"
#include "page_map.hpp"
#include "object_pool.hpp"

namespace tcm {

// 页缓存：按页数分桶管理空闲 Span，全局一把锁；缺 Span 时 SystemAlloc 并切分，
// 释放时经 PageMap 查前后相邻页是否空闲并循环合并。
class PageCache {
public:
    static PageCache* GetInstance() { return Immortal<PageCache>(); }

    // 申请 k 页的 Span
    Span* NewSpan(size_t k);

    // 释放一个 Span，尝试与前后相邻空闲页合并
    void  ReleaseSpanToPageCache(Span* span);

    // 由对象地址反查所属 Span：PageMap 读路径无锁，故释放快路径无需抢 PageCache 锁
    Span* MapObjectToSpan(void* ptr) const {
        return _pageMap->get(PageIDFromAddr(ptr));
    }

    // 空闲内存高水位（字节）：PageCache 中空闲 Span 总量超过它时，把整块空闲 chunk 归还 OS。
    // 默认 8MB；测试可调小以便快速触发。
    void   SetIdleReleaseLimit(size_t bytes) {
        std::lock_guard<std::mutex> lk(_mtx);
        _idleLimit = bytes;
    }
    size_t IdleFreeBytes() {
        std::lock_guard<std::mutex> lk(_mtx);
        return _freeBytes;
    }

private:
    void _registerSpan(Span* s) {
        for (size_t i = 0; i < s->_n; ++i) _pageMap->set(s->_pageId + i, s);
    }
    // 把 n >= k 的 span 切出头 k 页并标记在用，其余（若存在）并入空闲桶并登记
    void _splitReturn(Span* span, size_t k);

    // 空闲桶出入统一入口：同步维护 _freeBytes 记账
    void _pushFree(Span* s)  { _spanLists[s->_n].PushFront(s); _freeBytes += s->ByteSize(); }
    void _eraseFree(Span* s) { _spanLists[s->_n].Erase(s);    _freeBytes -= s->ByteSize(); }

    // 空闲量超过高水位时，把「完整 SystemAlloc 整块」的 Span 归还 OS（调用方须持锁）
    void _tryReleaseToOS();

    PageMap*         _pageMap = PageMap::GetInstance();
    SpanList         _spanLists[MAX_PAGES + 1];   // 桶 i = 空闲 i 页的 Span
    ObjectPool<Span> _spanPool;
    size_t           _freeBytes = 0;              // 空闲桶中累计字节数
    size_t           _idleLimit = 8 * 1024 * 1024;// 空闲高水位：超过则归还 OS
    std::mutex       _mtx;
};

} // namespace tcm