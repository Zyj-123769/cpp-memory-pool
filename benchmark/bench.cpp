// 内存池基准：对 8B / 64B / 256B / 4KB 四档做「全量分配 → 全量释放」的并发压测，
// 并与系统 malloc/free 对比吞吐（同线程数、同迭代数、同访问模式，保证可比性）。
// 用法：
//   bench [nthread] [liveMB] [both|pool|malloc]
//     nthread  并发线程数，默认 16
//     liveMB   每档目标活跃内存上限（MB），用于按规格反推每线程迭代数，默认 256
//     mode     只跑内存池 / 只跑 malloc / 两者都跑（默认 both，便于单档隔离取峰值 RSS）
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include "../include/memory_pool.hpp"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

static size_t PeakRSS() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc = {};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.PeakWorkingSetSize;
    return 0;
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<size_t>(ru.ru_maxrss) * 1024;   // Linux: KB
#endif
}

// UsePool=true 走内存池 my_malloc/my_free；false 走系统 malloc/free。
// 两者访问模式完全一致（先全部分配并写满，再全部释放），仅分配器不同。
template <bool UsePool>
static void RunScenario(size_t size, int nthread, long niter, double& sec) {
    std::atomic<unsigned long long> sink{0};
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    ts.reserve(nthread);
    for (int t = 0; t < nthread; ++t) {
        ts.emplace_back([=, &sink]() {
            std::vector<void*> ptrs;
            ptrs.reserve(niter);
            unsigned long long local = 0;
            for (long i = 0; i < niter; ++i) {
                void* p = UsePool ? tcm::my_malloc(size) : std::malloc(size);
                if (!p) continue;
                std::memset(p, 0xAB, size);
                ptrs.push_back(p);
            }
            for (void* p : ptrs) {
                local ^= reinterpret_cast<unsigned long long>(p);   // 防止整段分配/释放在优化下被消除
                if (UsePool) tcm::my_free(p); else std::free(p);
            }
            if (UsePool) tcm::ThreadCache::Get()->Drain();          // 池：把本线程滞留块批量归还
            sink.fetch_xor(local, std::memory_order_relaxed);
        });
    }
    for (auto& th : ts) th.join();
    auto t1 = std::chrono::steady_clock::now();
    sec = std::chrono::duration<double>(t1 - t0).count();
}

int main(int argc, char** argv) {
    const int    nthread = argc > 1 ? std::atoi(argv[1]) : 16;
    const int    liveMB  = argc > 2 ? std::atoi(argv[2]) : 256;
    const std::string mode = argc > 3 ? argv[3] : "both";
    const size_t sizes[] = {8, 64, 256, 4096};

    std::printf("== tcmalloc-like memory pool benchmark ==\n");
    std::printf("threads=%d  liveBudget=%dMB  pattern=alloc-all-then-free-all\n\n", nthread, liveMB);
    std::printf("%-6s %-11s %-9s %-16s %-16s %-8s\n",
                "size", "niter/thr", "liveMB", "pool", "malloc", "speedup");
    std::printf("%-6s %-11s %-9s %-16s %-16s %-8s\n",
                "", "", "", "(M ops/s)", "(M ops/s)", "");

    for (size_t sz : sizes) {
        long niter = (long)((double)liveMB * 1024 * 1024 / ((double)sz * nthread));
        if (niter < 1)         niter = 1;
        if (niter > 300000)    niter = 300000;             // 8B 档封顶，避免迭代数过大
        double live = (double)sz * niter * nthread / (1024.0 * 1024.0);

        double sp = 0, sm = 0;
        if (mode != "malloc") RunScenario<true >(sz, nthread, niter, sp);
        if (mode != "pool")   RunScenario<false>(sz, nthread, niter, sm);

        unsigned long long total = (unsigned long long)nthread * (unsigned long long)niter;
        double tp = sp > 0 ? (double)total / sp / 1e6 : 0.0;
        double tm = sm > 0 ? (double)total / sm / 1e6 : 0.0;
        double ratio = (tp > 0 && tm > 0) ? tp / tm : 0.0;

        char csize[16];
        if (sz >= 1024) std::snprintf(csize, sizeof(csize), "%zuKB", sz / 1024);
        else            std::snprintf(csize, sizeof(csize), "%zuB", sz);

        char cpool[16]  = "-", cmal[16] = "-", cratio[16] = "-";
        if (tp > 0) std::snprintf(cpool,  sizeof(cpool),  "%.1f", tp);
        if (tm > 0) std::snprintf(cmal,   sizeof(cmal),   "%.1f", tm);
        if (ratio > 0) std::snprintf(cratio, sizeof(cratio), "%.2fx", ratio);

        std::printf("%-6s %-11ld %-9.1f %-16s %-16s %-8s\n",
                    csize, niter, live, cpool, cmal, cratio);
    }

    std::printf("\npeak RSS = %.2f MB\n", (double)PeakRSS() / (1024.0 * 1024.0));
    return 0;
}
