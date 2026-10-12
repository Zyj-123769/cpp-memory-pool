#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <atomic>
#include "../include/memory_pool.hpp"
using namespace tcm;

int main(int argc, char** argv) {
    const int NTHREAD = argc > 1 ? std::atoi(argv[1]) : 8;
    const int NITER   = argc > 2 ? std::atoi(argv[2]) : 200000;   // 每线程
    const int SZMAX   = argc > 3 ? std::atoi(argv[3]) : 64;       // 规格档数（8..SZMAX*8）

    std::atomic<bool> ok{true};
    std::vector<std::thread> ts;
    ts.reserve(NTHREAD);
    for (int t = 0; t < NTHREAD; ++t) {
        ts.emplace_back([&ok, NITER, SZMAX]() {
            std::vector<void*> ptrs;
            ptrs.reserve(NITER);
            for (int i = 0; i < NITER; ++i) {
                size_t sz = 8 + (i % SZMAX) * 8;
                void* p = my_malloc(sz);
                if (!p) { ok = false; continue; }
                std::memset(p, 0x5A, sz);
                ptrs.push_back(p);
            }
            for (void* p : ptrs) my_free(p);
        });
    }
    for (auto& th : ts) th.join();
    if (ok) std::printf("test_thread: ALL PASS (threads=%d, per-thread=%d)\n", NTHREAD, NITER);
    else    std::printf("test_thread: FAILED\n");
    return ok ? 0 : 1;
}