#pragma once
#include <cstddef>

namespace tcm {

// 侵入式单链表：空闲块头部 8 字节存「下一块」指针
class FreeList {
public:
    FreeList() : _freeList(nullptr), _maxSize(1), _size(0) {}

    void Push(void* obj) {
        *reinterpret_cast<void**>(obj) = _freeList;
        _freeList = obj;
        ++_size;
    }

    void* Pop() {
        if (!_freeList) return nullptr;
        void* obj = _freeList;
        _freeList = *reinterpret_cast<void**>(obj);
        --_size;
        return obj;
    }

    // 把一段已链接好的节点 start..end 整体挂到表头
    void PushRange(void* start, void* end, size_t n) {
        if (!start || n == 0) return;
        *reinterpret_cast<void**>(end) = _freeList;
        _freeList = start;
        _size += n;
    }

    // 弹出 n 个节点（调用方保证 0 < n <= Size()）
    void PopRange(void*& start, void*& end, size_t n) {
        start = _freeList;
        end   = _freeList;
        for (size_t i = 1; i < n; ++i) end = *reinterpret_cast<void**>(end);
        _freeList = *reinterpret_cast<void**>(end);
        *reinterpret_cast<void**>(end) = nullptr;
        _size -= n;
    }

    bool   Empty()   const { return _freeList == nullptr; }
    size_t Size()    const { return _size; }
    size_t MaxSize() const { return _maxSize; }
    void   SetMaxSize(size_t m) { _maxSize = m; }
    void   Clear() { _freeList = nullptr; _size = 0; _maxSize = 1; }

private:
    void*  _freeList;
    size_t _maxSize;   // 慢启动阈值：链表长度超过它才批量归还 CentralCache
    size_t _size;      // 当前节点数
};

} // namespace tcm