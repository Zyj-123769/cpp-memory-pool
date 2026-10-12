#include <cstring>
#include <cstdio>
#include "../include/memory_pool.hpp"
#include "check.hpp"
using namespace tcm;

int main() {
    // 小对象反复申请/释放，验证写读不越界
    for (int round = 0; round < 200; ++round) {
        void* p = my_malloc(64);
        std::memset(p, 0xAB, 64);
        my_free(p);
    }

    // 多种规格写读校验
    for (size_t sz = 1; sz <= 4096; sz += 37) {
        void* p = my_malloc(sz);
        std::memset(p, 0x5A, sz);
        unsigned char* c = static_cast<unsigned char*>(p);
        bool ok = true;
        for (size_t i = 0; i < sz; ++i) if (c[i] != 0x5A) { ok = false; break; }
        CHECK(ok);
        my_free(p);
    }

    // 大对象（> 256KB）
    const size_t bigsz = 1024 * 1024;   // 1MB
    void* big = my_malloc(bigsz);
    std::memset(big, 0x7E, bigsz);
    CHECK(static_cast<unsigned char*>(big)[0] == 0x7E);
    CHECK(static_cast<unsigned char*>(big)[bigsz - 1] == 0x7E);
    my_free(big);

    // 一次性分配并全部释放一批小对象（触发批量归还/回收路径）
    const int N = 10000;
    void* arr[10000];
    for (int i = 0; i < N; ++i) arr[i] = my_malloc(32);
    for (int i = 0; i < N; ++i) my_free(arr[i]);

    // 0 大小与 null 释放边界
    void* z = my_malloc(0);
    CHECK(z != nullptr);
    my_free(z);
    my_free(nullptr);

    // 主动清空主线程缓存
    ThreadCache::Get()->Drain();

    TEST_SUMMARY("test_sanity");
}