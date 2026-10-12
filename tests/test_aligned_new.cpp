#include <cstdint>
#include <cstdio>
#include <new>
#include "memory_pool.hpp"
#include "check.hpp"

// 仅用 C++17 编译：验证带对齐的 operator new/delete 全家桶
// （过对齐请求由内存池的「多申请 + 前置指针槽」方案满足）

struct alignas(64)  A64  { char x[64]; };
struct alignas(128) A128 { char x[128]; };
struct alignas(256) A256 { char x[256]; };

static bool Aligned(void* p, std::size_t a) {
    return (reinterpret_cast<std::uintptr_t>(p) % a) == 0;
}

int main() {
    // 过对齐对象：new / delete
    A64* a = new A64;
    CHECK(Aligned(a, 64));
    for (int i = 0; i < 64; ++i) reinterpret_cast<unsigned char*>(a)[i] = 0x5A;
    CHECK(reinterpret_cast<unsigned char*>(a)[63] == 0x5A);
    delete a;

    // 过对齐数组：new[] / delete[]（含数组 cookie）
    A128* b = new A128[3];
    CHECK(Aligned(b, 128));
    for (int i = 0; i < 3; ++i) b[i].x[0] = static_cast<char>(i);
    CHECK(b[2].x[0] == 2);
    delete[] b;

    A256* c = new A256;
    CHECK(Aligned(c, 256));
    delete c;

    // 直接用对齐版 operator new / delete（含超大对齐）
    void* d = ::operator new(100, std::align_val_t(4096));
    CHECK(Aligned(d, 4096));
    ::operator delete(d, std::align_val_t(4096));

    // nothrow 对齐版
    void* e = ::operator new(64, std::align_val_t(512), std::nothrow);
    CHECK(e != nullptr);
    CHECK(Aligned(e, 512));
    ::operator delete(e, std::align_val_t(512), std::nothrow);

    // 反复申请/释放，确认前置指针槽还原正确、无越界
    for (int r = 0; r < 2000; ++r) {
        A64* p = new A64;
        if (!Aligned(p, 64)) { CHECK(false); break; }
        delete p;
    }

    TEST_SUMMARY("test_aligned_new");
}
