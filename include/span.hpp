#pragma once
#include <atomic>
#include "common.hpp"
#include "freelist.hpp"

namespace tcm {

// Span：连续若干个页的分配单位
struct Span {
    PageID   _pageId   = 0;
    size_t   _n        = 0;      // 页数
    // 所属 SystemAlloc 整块的起始页号（整块固定为 MAX_PAGES 页）。
    // 用途：(1) 相邻页合并只允许在同一整块内进行，避免拼出跨块的 Span；
    //       (2) 只有「起点 == _chunkBase 且页数 == MAX_PAGES」的整块空闲 Span 才可整块归还 OS。
    PageID   _chunkBase = 0;
    Span*    _next     = nullptr;
    Span*    _prev     = nullptr;
    // 小对象的定长规格；0 表示大对象 Span。释放快路径无锁读取，故用原子类型：
    // 该字段在 Span 交付使用前写入、在 Span 在用期间不再变更，故 relaxed 读即可，
    // 与「对象交付」之间已由其他同步（CentralCache 锁/线程间传递）建立可见性。
    std::atomic<size_t> _objSize{0};
    size_t   _useCount = 0;      // 已分配出去（借出）的对象个数
    FreeList _freeList;          // 本 Span 内空闲对象链表
    bool     _isUse    = false;  // 是否在用；false 表示位于 PageCache 空闲桶（可合并）

    void*  PageStart() const { return AddrFromPageID(_pageId); }
    size_t ByteSize()  const { return _n << PAGE_SHIFT; }
};

// 双向循环链表（带头哨兵），同规格的一组 Span 挂在一条上
struct SpanList {
    SpanList() { _head._next = &_head; _head._prev = &_head; }

    bool  Empty() const { return _head._next == &_head; }
    Span* Begin() const { return _head._next; }
    Span* End()   const { return const_cast<Span*>(&_head); }

    void PushFront(Span* s) { Insert(&_head, s); }

    Span* PopFront() {
        if (Empty()) return nullptr;
        Span* s = _head._next;
        Erase(s);
        return s;
    }

    void Insert(Span* pos, Span* s) {
        s->_prev = pos;
        s->_next = pos->_next;
        pos->_next->_prev = s;
        pos->_next = s;
    }

    void Erase(Span* s) {
        s->_prev->_next = s->_next;
        s->_next->_prev = s->_prev;
        s->_next = s->_prev = nullptr;
    }

    Span _head;
};

} // namespace tcm