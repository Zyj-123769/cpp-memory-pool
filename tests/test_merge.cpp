#include "common.hpp"
#include "page_cache.hpp"
#include "check.hpp"
using namespace tcm;

// 验证 Span 的相邻页合并：释放后应拼回整块并命中复用，且不再向系统申请内存。
//
// PageCache 每次向系统要一整块 MAX_PAGES(128) 页并切分：
//   NewSpan(32) -> 得到 [base, base+32)，余 96 页入空闲桶
//   NewSpan(64) -> 从上一步的 96 页里切出 [base+32, base+96)，余 32 页入空闲桶
//   释放 a(32 页) 后，因 b 在用，暂不能合并
//   释放 b(64 页) 后，应分别向后(+32)、向前(+32)合并，拼回完整 128 页
//   再 NewSpan(128) 必须命中合并结果（起始页仍是 base），且不触发新的系统申请
int main() {
    PageCache* pc = PageCache::GetInstance();

    Span* a = pc->NewSpan(32);
    CHECK(a != nullptr);
    const PageID base = a->_pageId;

    // 记录此刻的系统申请次数：此后若合并成功，就不应再增加
    const size_t sysAfterFirst = SysAllocCalls().load();
    CHECK(sysAfterFirst >= 1);

    Span* b = pc->NewSpan(64);                 // 应从 a 之后切出，无需新申请
    CHECK(b != nullptr);
    CHECK(b->_pageId == base + 32);
    CHECK(SysAllocCalls().load() == sysAfterFirst);

    pc->ReleaseSpanToPageCache(a);             // b 在用，无法合并
    pc->ReleaseSpanToPageCache(b);             // 前/后相邻页皆空闲 -> 合并回 128 页

    Span* c = pc->NewSpan(128);
    CHECK(c != nullptr);
    CHECK(c->_pageId == base);                 // 起始页一致 => 复用合并结果，而非新 mmap
    CHECK(c->_n == 128);
    CHECK(SysAllocCalls().load() == sysAfterFirst);   // 关键：未触发新的系统申请

    TEST_SUMMARY("test_merge");
}
