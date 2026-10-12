#pragma once
#include <mutex>
#include "common.hpp"

namespace tcm {

// 定长对象池：从系统内存整体切块，绕过 operator new（防止与全局 new 重载互相递归）。
// 仅被 PageCache 在持有其锁的情况下调用，这里再自加一把互斥锁是稳妥做法。
template <typename T>
class ObjectPool {
public:
    T* New() {
        std::lock_guard<std::mutex> lk(_mtx);
        if (_freeList) {
            void* obj = _freeList;
            _freeList = *reinterpret_cast<void**>(_freeList);
            return new (obj) T();
        }
        size_t objSize = sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
        char* chunk = static_cast<char*>(SystemAllocBytes(kChunk * objSize));
        for (size_t i = 0; i < kChunk; ++i) {
            void* obj = chunk + i * objSize;
            *reinterpret_cast<void**>(obj) = _freeList;
            _freeList = obj;
        }
        void* obj = _freeList;
        _freeList = *reinterpret_cast<void**>(_freeList);
        return new (obj) T();
    }

    void Delete(T* obj) {
        if (!obj) return;
        obj->~T();
        std::lock_guard<std::mutex> lk(_mtx);
        *reinterpret_cast<void**>(obj) = _freeList;
        _freeList = obj;
    }

private:
    static const size_t kChunk = 4096;
    void*       _freeList = nullptr;
    std::mutex  _mtx;
};

} // namespace tcm