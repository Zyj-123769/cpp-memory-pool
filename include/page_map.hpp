#pragma once
#include <atomic>
#include "common.hpp"
#include "span.hpp"

namespace tcm {

// 基数树（三级）实现 PageID -> Span* 映射：查询 O(1)、无需哈希、无需扩容。
// 节点由系统内存分配并惰性创建。写路径仅由 PageCache 在其锁内调用（单写者），
// 读路径（释放对象时反查 Span）完全无锁：节点指针与叶表项均以 std::atomic +
// acquire/release 发布，因此多线程可并发读、无需持有 PageCache 锁。
class PageMap {
public:
    static PageMap* GetInstance() { return Immortal<PageMap>(); }

    PageMap() : _root(NewNode()) {}

    // 无锁读：任意线程可安全并发调用（释放快路径的关键）
    Span* get(PageID id) const {
        size_t i1 = (id >> (L2_BITS + L3_BITS)) & L_MASK;
        size_t i2 = (id >> L3_BITS) & L_MASK;
        size_t i3 = id & L_MASK;

        void* l2v = _root[i1].load(std::memory_order_acquire);
        if (!l2v) return nullptr;
        auto* l2 = static_cast<std::atomic<void*>*>(l2v);

        void* l3v = l2[i2].load(std::memory_order_acquire);
        if (!l3v) return nullptr;
        auto* l3 = static_cast<std::atomic<void*>*>(l3v);

        return static_cast<Span*>(l3[i3].load(std::memory_order_acquire));
    }

    // 写：仅由 PageCache 在其锁内调用，故写者之间天然串行，无需额外同步
    void set(PageID id, Span* s) {
        size_t i1 = (id >> (L2_BITS + L3_BITS)) & L_MASK;
        size_t i2 = (id >> L3_BITS) & L_MASK;
        size_t i3 = id & L_MASK;

        auto* l2 = GetOrCreate(_root[i1]);
        auto* l3 = GetOrCreate(l2[i2]);
        l3[i3].store(s, std::memory_order_release);
    }

private:
    enum { L1_BITS = 13, L2_BITS = 13, L3_BITS = 13 };
    enum { L_MASK = (1 << 13) - 1 };
    enum { NENTRY = 1 << 13 };

    using Node = std::atomic<void*>;

    // 分配并初始化一节点（SystemAllocBytes 已清零，再显式构造原子对象）
    static Node* NewNode() {
        Node* n = static_cast<Node*>(SystemAllocBytes(sizeof(Node) * NENTRY));
        for (size_t i = 0; i < NENTRY; ++i) new (&n[i]) Node(nullptr);
        return n;
    }

    static Node* GetOrCreate(std::atomic<void*>& slot) {
        void* v = slot.load(std::memory_order_acquire);
        if (v) return static_cast<Node*>(v);
        Node* n = NewNode();
        slot.store(n, std::memory_order_release);
        return n;
    }

    Node* _root;
};

} // namespace tcm
