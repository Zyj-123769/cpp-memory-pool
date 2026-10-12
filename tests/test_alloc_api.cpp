#include <cstdio>
#include <cstring>
#include <new>
#include "memory_pool.hpp"
#include "check.hpp"
using namespace tcm;

// 验证 calloc/realloc/usable_size 与全局 operator new/delete 重载
int main() {
    // ---- calloc：零初始化 + usable_size >= 请求量 ----
    int* a = static_cast<int*>(my_calloc(16, sizeof(int)));
    CHECK(a != nullptr);
    bool zeroed = true;
    for (int i = 0; i < 16; ++i) if (a[i] != 0) zeroed = false;
    CHECK(zeroed);
    CHECK(my_usable_size(a) >= 16 * sizeof(int));

    // ---- realloc 扩容保留原有数据 ----
    for (int i = 0; i < 16; ++i) a[i] = i + 1;
    a = static_cast<int*>(my_realloc(a, 64 * sizeof(int)));
    CHECK(a != nullptr);
    bool kept = true;
    for (int i = 0; i < 16; ++i) if (a[i] != i + 1) kept = false;
    CHECK(kept);
    CHECK(my_usable_size(a) >= 64 * sizeof(int));
    my_free(a);

    // ---- 同档 realloc 应原地复用（返回同一指针）----
    void* p = my_malloc(64);
    void* q = my_realloc(p, 60);        // 60 与 64 同属 64 档
    CHECK(q == p);
    my_free(q);

    // ---- realloc(ptr, 0) 语义：释放并返回空 ----
    void* z = my_malloc(32);
    CHECK(my_realloc(z, 0) == nullptr);

    // ---- realloc(nullptr, n) == malloc(n) ----
    void* r = my_realloc(nullptr, 24);
    CHECK(r != nullptr);
    CHECK(my_usable_size(r) >= 24);
    my_free(r);

    // ---- operator new / delete：证明 new 走了内存池（usable_size 非 0）----
    int* arr = new int[100];
    CHECK(arr != nullptr);
    CHECK(my_usable_size(arr) >= 100 * sizeof(int));   // 未被替换则返回 0，断言失败
    for (int i = 0; i < 100; ++i) arr[i] = i;
    CHECK(arr[99] == 99);
    delete[] arr;

    struct Big { char buf[512]; };
    Big* b = new Big;
    CHECK(my_usable_size(b) >= sizeof(Big));
    std::memset(b, 0x3C, sizeof(Big));
    CHECK(b->buf[511] == 0x3C);
    delete b;

    // ---- nothrow 变体 ----
    void* nb = ::operator new(128, std::nothrow);
    CHECK(nb != nullptr);
    CHECK(my_usable_size(nb) >= 128);
    ::operator delete(nb);

    // ---- 大对象（> 256KB）走 new/delete ----
    const size_t bigsz = 512 * 1024;
    void* big = ::operator new(bigsz);
    CHECK(big != nullptr);
    CHECK(my_usable_size(big) >= bigsz);
    std::memset(big, 0x11, bigsz);
    ::operator delete(big);

    // ---- delete nullptr 安全 ----
    int* nul = nullptr;
    delete nul;

    TEST_SUMMARY("test_alloc_api");
}
