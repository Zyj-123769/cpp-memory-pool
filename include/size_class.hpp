#pragma once
#include <cstddef>
#include "common.hpp"

namespace tcm {

// 请求字节数 -> 定长规格 的映射（向上取整）。
// 梯度分段（小对象密、大对象疏）：
//   [1,1KB]        16B  对齐  -> 64 档   （SizeClass(17)=32、SizeClass(129)=144 均落在本段）
//   (1KB,8KB]      128B 对齐  -> 56 档
//   (8KB,64KB]     1KB  对齐  -> 56 档
//   (64KB,256KB]   8KB  对齐  -> 24 档
struct SizeClass {
    static constexpr size_t NCLASS = 200;   // 64 + 56 + 56 + 24

    static size_t RoundUp(size_t size) {
        if (size <= 1024)       return _RoundUp(size, 16);
        if (size <= 8 * 1024)   return _RoundUp(size, 128);
        if (size <= 64 * 1024)  return _RoundUp(size, 1024);
        if (size <= 256 * 1024) return _RoundUp(size, 8 * 1024);
        return _RoundUp(size, PAGE_SIZE);
    }

    // 请求字节数 -> 自由链表桶下标
    static size_t Index(size_t size) {
        if (size <= 1024)       return _Index(size, 4);
        if (size <= 8 * 1024)   return _Index(size - 1024, 7) + 64;
        if (size <= 64 * 1024)  return _Index(size - 8 * 1024, 10) + 120;
        if (size <= 256 * 1024) return _Index(size - 64 * 1024, 13) + 176;
        return 0;   // 大对象，另行处理
    }

    // 桶下标 -> 该档定长规格（Index 的逆映射）
    static size_t Classes(size_t index) {
        if (index < 64)  return (index + 1) * 16;
        if (index < 120) return 1024 + (index - 64 + 1) * 128;
        if (index < 176) return 8 * 1024 + (index - 120 + 1) * 1024;
        return 64 * 1024 + (index - 176 + 1) * 8 * 1024;
    }

    // 一次批量领取对象数：小对象多取、大对象少取，摊薄跨层锁成本，并控制单次滞留量
    static size_t NumMoveSize(size_t size) {
        if (size == 0) return 0;
        size_t num = MAX_BYTES / size;
        if (num < 2)   num = 2;
        if (num > 512) num = 512;
        return num;
    }

    // 一次批量领取所需的页数
    static size_t NumMovePage(size_t size) {
        size_t num   = NumMoveSize(size);
        size_t npage = (num * size) >> PAGE_SHIFT;
        if (npage == 0) npage = 1;
        return npage;
    }

private:
    static size_t _RoundUp(size_t size, size_t align) {
        return (size + align - 1) & ~(align - 1);
    }
    static size_t _Index(size_t size, size_t alignShift) {
        return ((size + (size_t(1) << alignShift) - 1) >> alignShift) - 1;
    }
};

} // namespace tcm