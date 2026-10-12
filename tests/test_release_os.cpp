#include <cstdio>
#include <vector>
#include "memory_pool.hpp"
#include "page_cache.hpp"
#include "check.hpp"
using namespace tcm;

// 验证空闲 Span 归还 OS：把 PageCache 高水位调小，分配并释放大量小对象后，
// 攒下的整块空闲 chunk 应被真正 munmap/VirtualFree 还给系统，且空闲量被压回水位以下。
int main() {
    PageCache* pc = PageCache::GetInstance();
    pc->SetIdleReleaseLimit(4 * 1024 * 1024);      // 高水位 4MB，便于快速触发

    const int N = 300000;                          // 64B * 300000 ≈ 18.75MB，跨越多个 chunk
    std::vector<void*> v;
    v.reserve(N);                                  // 大对象缓冲走 large 路径，不占用小对象 chunk

    for (int i = 0; i < N; ++i) v.push_back(my_malloc(64));

    const size_t freeBefore = SysFreeCalls().load();   // 释放前的归还次数（此工作负载下应≈0）
    for (int i = 0; i < N; ++i) my_free(v[i]);
    ThreadCache::Get()->Drain();                   // 把线程缓存滞留的块也还回 CentralCache/PageCache
    const size_t freeAfter = SysFreeCalls().load();

    CHECK(freeAfter > freeBefore);                 // 确实发生了归还 OS
    CHECK(pc->IdleFreeBytes() <= 4 * 1024 * 1024); // 空闲量已被压回高水位以下

    TEST_SUMMARY("test_release_os");
}
