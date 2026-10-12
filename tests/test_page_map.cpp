#include "../include/page_map.hpp"
#include "check.hpp"
using namespace tcm;

int main() {
    PageMap* pm = PageMap::GetInstance();

    Span s1; s1._pageId = 1000; s1._n = 4;
    Span s2; s2._pageId = 5;    s2._n = 1;
    for (size_t i = 0; i < s1._n; ++i) pm->set(s1._pageId + i, &s1);
    pm->set(s2._pageId, &s2);

    CHECK(pm->get(1000) == &s1);
    CHECK(pm->get(1003) == &s1);
    CHECK(pm->get(5) == &s2);
    CHECK(pm->get(999) == nullptr);
    CHECK(pm->get(1004) == nullptr);

    // 模拟高地址（39 位页号范围内），覆盖多级基数树路径
    PageID high = (PageID(1) << 35) - 1;
    Span sb; sb._pageId = high; sb._n = 1;
    pm->set(high, &sb);
    CHECK(pm->get(high) == &sb);
    CHECK(pm->get(high - 1) == nullptr);

    // 覆盖后读取
    pm->set(1000, &sb);
    CHECK(pm->get(1000) == &sb);

    TEST_SUMMARY("test_page_map");
}