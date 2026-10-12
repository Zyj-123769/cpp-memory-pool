#include "../include/size_class.hpp"
#include "check.hpp"
using namespace tcm;

int main() {
    // Phase 1 核心断言（与 md 示例一致）
    CHECK(SizeClass::RoundUp(1)  == 16);
    CHECK(SizeClass::RoundUp(16) == 16);
    CHECK(SizeClass::RoundUp(17) == 32);    // md 文档示例：17 -> 32
    CHECK(SizeClass::RoundUp(128) == 128);
    CHECK(SizeClass::RoundUp(129) == 144);  // md 文档示例：129 -> 144
    CHECK(SizeClass::RoundUp(1024) == 1024);
    CHECK(SizeClass::RoundUp(1025) == 1152);
    CHECK(SizeClass::RoundUp(256 * 1024) == 256 * 1024);
    CHECK(SizeClass::Index(17) == 1);
    CHECK(SizeClass::Index(129) == 8);

    // Index 与 Classes 互逆 + 对齐不丢精度
    for (size_t s = 1; s <= 256 * 1024; s = s + 1 + (s / 7)) {
        size_t idx = SizeClass::Index(s);
        size_t cls = SizeClass::Classes(idx);
        CHECK(cls >= s);
        CHECK(SizeClass::Index(cls) == idx);
    }

    // NumMoveSize 界
    CHECK(SizeClass::NumMoveSize(8) == 512);
    CHECK(SizeClass::NumMoveSize(256 * 1024) == 2);
    CHECK(SizeClass::NumMovePage(8) == 1);
    CHECK(SizeClass::NumMovePage(64) == 4);

    // 内部碎片率统计（平均应低于阈值）
    unsigned long long total_req = 0, total_alloc = 0;
    for (size_t s = 1; s <= 256 * 1024; ++s) {
        total_req   += s;
        total_alloc += SizeClass::RoundUp(s);
    }
    double waste = (double)(total_alloc - total_req) / (double)total_req;
    std::printf("internal fragmentation rate: %.3f%%\n", waste * 100.0);
    CHECK(waste < 0.15);

    TEST_SUMMARY("test_size_class");
}