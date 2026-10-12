#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../include/memory_pool.hpp"
using namespace tcm;

int main(int argc, char** argv) {
    const int ROUND = argc > 1 ? std::atoi(argv[1]) : 1000;
    const int N     = argc > 2 ? std::atoi(argv[2]) : 128;
    std::vector<void*> pool;
    pool.reserve(N);

    for (int round = 0; round < ROUND; ++round) {
        for (int i = 0; i < N; ++i) {
            size_t sz = 8 + (i % 64) * 8;    // 8..512
            void* p = my_malloc(sz);
            if (!p) { std::printf("null alloc at round=%d i=%d\n", round, i); return 1; }
            std::memset(p, 0x5A, sz);
            pool.push_back(p);
        }
        for (void* p : pool) my_free(p);
        pool.clear();
    }
    ThreadCache::Get()->Drain();
    std::printf("interleave: ALL PASS\n");
    return 0;
}