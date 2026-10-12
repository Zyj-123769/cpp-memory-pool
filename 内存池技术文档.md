# 内存池（仿 tcmalloc）技术文档

> 版本：与当前 `main` 工作区代码一致 ｜ 语言：C++14（部分能力需 C++17）｜ 命名空间：`tcm`
> 定位：一个对标 Google tcmalloc 的三级缓存内存分配器，可用于替换 C++ 的 `new/delete`。

---

## 1. 概述

### 1.1 目标

- 用「线程缓存 → 中心缓存 → 页缓存」三级结构，把小对象（≤ 256KB）的分配/释放做成**线程内无锁**操作。
- 以 Span（连续若干页）为管理单位，支持拆分与相邻页合并，治理外部碎片。
- 提供与 `malloc/free/calloc/realloc/malloc_usable_size` 同语义的接口，并全局替换 C++ `operator new/delete`。

### 1.2 关键指标

| 项目 | 取值 |
|---|---|
| 页大小 `PAGE_SIZE` | 8 KB（`1 << 13`） |
| 小对象上限 `MAX_BYTES` | 256 KB |
| 单块最大页数 `MAX_PAGES` | 128 页（= 1 MB，即一次 `SystemAlloc` 的粒度） |
| size class 档数 `NCLASS` | 200 |
| 内部碎片率（1B…256KB 平均，实测） | ≈ 2.43% |
| 空闲归还高水位 `_idleLimit` | 默认 8 MB（可调） |

### 1.3 与 tcmalloc 的关系

三级缓存骨架、自由链表批量领取/归还、Span 合并、基数树 PageMap 均对齐 tcmalloc 的核心设计；本实现是教学化简版，未包含 tcmalloc 的 per-CPU 缓存、背景回收线程、采样统计等高级特性。

---

## 2. 总体架构

```
             my_malloc / operator new                my_free / operator delete
                    │                                        │
                    ▼                                        ▼
        ┌───────────────────────┐               ┌────────────────────────────┐
        │      ThreadCache      │  thread_local │  PageMap（三级基数树，无锁读）│
        │  200 条自由链表（TLS） │◀── 无锁 ───────│  由对象地址反查所属 Span      │
        └───────────┬───────────┘               └────────────┬───────────────┘
        桶空↓批量领   ↑桶满批量还                              │ 得到 _objSize
        ┌───────────▼───────────┐                            │
        │     CentralCache      │  每档一把锁                 │
        │   每规格一条 SpanList  │                            │
        └───────────┬───────────┘                            │
        Span 用尽↓取新  ↑计数归零整块退回                       │
        ┌───────────▼───────────┐                            │
        │      PageCache        │  全局一把锁                 │
        │  1..128 页分桶 + 合并   │◀───────────────────────────┘
        └───────────┬───────────┘
        缺块↓SystemAlloc  ↑空闲整块超过水位则 SystemFree
        ┌───────────▼───────────┐
        │   OS（VirtualAlloc / mmap）│
        └───────────────────────┘
```

**一次小对象分配**：`ThreadCache::Allocate` 从本线程对应档自由链表取头节点；链表空则向 `CentralCache` 批量领一段；`CentralCache` 无可用 Span 时向 `PageCache` 取一个新 Span 并切块。

**一次小对象释放**：无锁由 `PageMap` 反查所属 Span → 读 `Span._objSize` 得到档位 → 挂回本线程对应档自由链表；链表过长则批量还给 `CentralCache`；`CentralCache` 中某 Span 借出计数归零时整块退回 `PageCache`；`PageCache` 空闲量超过水位时把完整整块 `SystemFree` 给 OS。

---

## 3. 基础约定（common.hpp）

### 3.1 常量与地址换算

```cpp
PAGE_SHIFT = 13;  PAGE_SIZE = 8KB;  MAX_BYTES = 256KB;  MAX_PAGES = 128;
PageID PageIDFromAddr(void* p);   // 地址 >> 13，得到页号
void*  AddrFromPageID(PageID id); // 页号 << 13，得到地址
```

页号 `PageID` 用 `uintptr_t` 表示。

### 3.2 系统内存申请/归还

| 平台 | 申请 | 归还 |
|---|---|---|
| Windows | `VirtualAlloc(nullptr, bytes, MEM_RESERVE｜MEM_COMMIT, PAGE_READWRITE)` | `VirtualFree(p, 0, MEM_RELEASE)` |
| Linux | `mmap(nullptr, bytes, PROT_READ｜PROT_WRITE, MAP_PRIVATE｜MAP_ANONYMOUS, -1, 0)` | `munmap(p, bytes)` |

- `SystemAlloc(npages)`：按页申请，成功时对 `SysAllocCalls()` 计数 +1。
- `SystemFree(p, npages)`：归还，成功时对 `SysFreeCalls()` 计数 +1。
- `SystemAllocBytes(bytes)`：按字节申请（向上取整到页），**仅供内部结构使用，生命周期为进程级、不归还**。
- 两个计数器供测试验证「合并后未再申请」「空闲归还确实发生」。

> 关键点：内部结构（PageMap 节点、ObjectPool 切块）一律走 `SystemAlloc`，**不经过 `operator new`**，因此与全局重载的 `operator new` 不会互相递归。

### 3.3 永不析构的单例 `Immortal<T>`

```cpp
template <typename T> T* Immortal() {
    static aligned_storage<sizeof(T), alignof(T)>::type storage;
    static T* instance = new (&storage) T();   // placement new，走静态存储，不调用 operator new
    return instance;
}
```

用于 `PageMap / CentralCache / PageCache`。规避两类问题：(1) 单例初始化时调用重载后的 `operator new` 造成递归；(2) 静态析构顺序不确定导致「单例已析构又被访问」。

---

## 4. 核心数据结构

### 4.1 FreeList（侵入式自由链表）

空闲块头部 8 字节存放「下一块」指针，零额外存储。

| 方法 | 语义 |
|---|---|
| `Push` / `Pop` | 头插、取头 |
| `PushRange(start,end,n)` | 把已链接好的一段整体挂到头 |
| `PopRange(start,end,n)` | 弹出前 n 个（`0 < n ≤ Size()`） |
| `Empty/Size/MaxSize/SetMaxSize/Clear` | 查询与慢启动阈值管理 |

`_maxSize` 是**慢启动阈值**：链表长度达到它才触发批量归还。

### 4.2 Span 与 SpanList

```cpp
struct Span {
    PageID   _pageId;     // 起始页号
    size_t   _n;          // 页数
    PageID   _chunkBase;  // 所属 SystemAlloc 整块起始页号（整块固定 MAX_PAGES 页）
    Span*    _next, *_prev;
    std::atomic<size_t> _objSize;  // 小对象定长规格；0 表示大对象 Span
    size_t   _useCount;   // 已借出对象数
    FreeList _freeList;   // 本 Span 内空闲对象链表
    bool     _isUse;      // true=在用；false=位于 PageCache 空闲桶（可合并）
};
```

- `_objSize` 为**原子类型**：它在 Span 交付使用前写入、在用期间不再变更，释放快路径可 `relaxed` 无锁读取。
- `_chunkBase` 是「同整块合并」与「整块归还 OS」两个机制的基础（见 §5.3）。
- `SpanList` 为带头哨兵的双向循环链表，同规格的一组 Span 挂在一条上。

### 4.3 SizeClass（字节 → 定长规格）

分段对齐，**小对象密、大对象疏**：

| 区间 | 对齐 | 档数 | 档位示例 |
|---|---|---|---|
| (0, 1KB] | 16 B | 64 | 16, 32, …, 1024（`RoundUp(17)=32`） |
| (1KB, 8KB] | 128 B | 56 | 1152, 1280, …, 8192（`RoundUp(129)=144`） |
| (8KB, 64KB] | 1 KB | 56 | 9216, …, 65536 |
| (64KB, 256KB] | 8 KB | 24 | 73728, …, 262144 |

配套函数：

- `RoundUp(size)`：向上对齐到定长规格（**保证同一档所有块等长**）。
- `Index(size)`：字节 → 桶下标；`Classes(index)`：桶下标 → 规格（两者互逆）。
- `NumMoveSize(size) = clamp(MAX_BYTES / size, 2, 512)`：一次批量领取的对象数（小对象多取、大对象少取）。
- `NumMovePage(size) = max(1, (NumMoveSize*size) >> 13)`：一次批量领取所需的页数。

### 4.4 PageMap（PageID → Span*，三级基数树）

- 三级各 13 位索引（`L1=L2=L3=13`，`L_MASK=8191`），覆盖 39 位页号 ≈ 2^52 字节地址空间。
- 节点惰性创建；**叶表项与节点指针均为 `std::atomic<void*>`**。
  - 写 `set()`：仅由 PageCache 在其锁内调用（单写者），叶表项以 `memory_order_release` 发布，节点指针同理。
  - 读 `get()`：**完全无锁**，逐级 `memory_order_acquire` 加载。这是释放快路径无锁的关键。

```cpp
Span* get(PageID id) const {                 // 无锁，可多线程并发
    void* l2v = _root[i1].load(acquire);  if (!l2v) return nullptr;
    auto* l2  = static_cast<std::atomic<void*>*>(l2v);
    void* l3v = l2[i2].load(acquire);     if (!l3v) return nullptr;
    auto* l3  = static_cast<std::atomic<void*>*>(l3v);
    return static_cast<Span*>(l3[i3].load(acquire));
}
```

### 4.5 ObjectPool（定长对象池）

为 `Span` 提供对象，`_spanPool.New()/_spanPool.Delete()`：

- 从系统按整块（`kChunk = 4096` 个对象）切分，块内串成自由链表复用。
- 用 placement new 构造、显式析构；**绕过 `operator new`**。
- 自带一把互斥锁（即便调用方已持 PageCache 锁，稳妥再保护一层）。

---

## 5. 三级缓存详解

### 5.1 ThreadCache（线程缓存，TLS）

```cpp
static ThreadCache* Get() { static thread_local ThreadCache tc; return &tc; }
```

- 每个线程持有 `FreeList _freeLists[200]`（约几 KB），按 size class 分桶。
- **分配** `Allocate(size)`：`RoundUp` → `Index` → 取头节点；桶空则 `FetchFromCentralCache`。
- **释放** `Deallocate(ptr,size)`：`Push` 回桶；若 `Size() ≥ MaxSize()` 则 `ListTooLong` 批量归还。
- **慢启动**：`MaxSize` 从 1 起，每次跨层领取 +1，直到不超过 `NumMoveSize`。
  ```cpp
  size_t batchNum = min(MaxSize, NumMoveSize(size));
  if (MaxSize == batchNum) SetMaxSize(batchNum + 1);
  ```
  这样首个对象只领 1 个，随使用量逐步放大批量，避免一次性滞留过多内存。
- **线程退出** `~ThreadCache(){ Drain(); }`：把本线程滞留块全部归还，避免误报泄漏。

> 分配/释放的**常见路径完全无锁**：只操作本线程链表 + 无锁 PageMap 读。

### 5.2 CentralCache（中心缓存）

- 每规格一条 `SpanList` + **一把独立锁** `_mtxList[200]`（锁粒度细分，降低竞争）。

| 方法 | 说明 |
|---|---|
| `GetOneSpan(list,size)` | 在 list 内找仍有空闲对象的 Span；没有则 `PageCache::NewSpan(NumMovePage)` 取新 Span，按 `size` 切块挂成自由链表。**OOM 时返回 nullptr。** |
| `FetchRangeObj(start,end,batchNum,size)` | 从 Span 自由链表切出 `min(batchNum, 可用数)` 个，累加 `_useCount`；返回实际数量（OOM 时返回 0）。 |
| `ReleaseListToSpans(start,size)` | 逐块由 `PageMap` 反查所属 Span 挂回并 `--_useCount`；**计数归 0 时整块退回 `PageCache`**。 |

### 5.3 PageCache（页缓存）

- 以页数 `1..128` 分桶管理空闲 Span，**全局一把锁** `_mtx`。
- 空闲记账：`_freeBytes` 由统一的 `_pushFree` / `_eraseFree` 入口维护。

**申请 `NewSpan(k)`**：

1. `k > MAX_PAGES`（大对象）：直接 `SystemAlloc(k)`，只登记首页到 PageMap，`_objSize = 0` 标记大对象。
2. `k ≤ MAX_PAGES`：优先取 `_spanLists[k]`；否则向上找最小的非空桶；都没有则 `SystemAlloc(MAX_PAGES)` 拿一整块。
3. `_splitReturn(span,k)`：切出头 k 页标记在用，其余作为尾块入空闲桶（继承 `_chunkBase`）。

**释放 `ReleaseSpanToPageCache(span)`**：

1. 大对象：清空其页映射后 `SystemFree`。
2. 小对象：标记空闲 → 向前/向后合并相邻**同整块**的空闲页 → 入桶 → `_tryReleaseToOS()`。

**合并约束（关键）**：只允许合并「`_chunkBase` 相同」的相邻空闲页，且合并后 ≤ `MAX_PAGES`。

```
整块 A: [base ....... base+128)          整块 B: [base+128 ... )
         └── 同 _chunkBase 才可互相合并 ──┘   ← 绝不跨整块合并
```

这条约束保证任何合并结果的起止都落在**同一次 SystemAlloc** 的区间内 —— 这是「整块归还 OS」得以安全进行的前提（`munmap` / `VirtualFree(MEM_RELEASE)` 只能释放一次完整分配的整块）。

**归还 OS `_tryReleaseToOS()`**：当 `_freeBytes > _idleLimit`，把满足「起点 == `_chunkBase` 且 `_n == MAX_PAGES`」的完整整块 Span 清映射后 `SystemFree`，直到降到水位以下。这类 Span 恰好落在 `_spanLists[MAX_PAGES]`。

- 高水位默认 8 MB，可通过 `SetIdleReleaseLimit(bytes)` 调整；`IdleFreeBytes()` 观测当前空闲字节数。

---

## 6. 关键机制

### 6.1 释放快路径为何无锁

```
my_free(ptr):
  span  = PageCache::MapObjectToSpan(ptr)   // PageMap 原子读，无锁
  objSz = span->_objSize.load(relaxed)      // 原子读，无锁（Span 在用期间该值不变）
  if (objSz == 0) 大对象 → ReleaseSpanToPageCache()   // 加锁慢路径（罕见）
  else           ThreadCache::Deallocate(ptr, objSz)  // 挂回 TLS 链表，无锁
```

只有「批量归还」和「整块回收/归还 OS」才进入加锁慢路径，且是**每 N 次释放一次**（N 由慢启动与容量上限决定），把锁竞争从「每次释放」摊薄到「每 N 次释放」。

### 6.2 批量申请/归还的数学

- 一次领取 `NumMoveSize` 个而非 1 个，摊薄跨层锁成本；`NumMoveSize` 随规格增大而递减（`MAX_BYTES/size`，下限 2、上限 512）。
- 领取所需页数 `NumMovePage = (NumMoveSize*size) >> 13`，使一次领取恰好铺满若干页。
- 归还时若本线程桶长度超阈值，整段还回 `CentralCache`，控制每线程滞留量。

### 6.3 并发与锁序

| 层 | 同步方式 |
|---|---|
| ThreadCache | 无锁（TLS，仅本线程访问） |
| PageMap 读 | 无锁（原子 acquire） |
| CentralCache | 每规格一把锁 `_mtxList[i]` |
| PageCache | 全局一把锁 `_mtx` |

**锁序固定为：`_mtxList[i]` → `_mtx`。** `CentralCache` 在持档位锁时会调用 `PageCache::NewSpan / ReleaseSpanToPageCache`（取 `_mtx`）；`PageCache` 不会回调 `CentralCache`，故无环形等待。释放快路径只取无锁读，不参与该锁序。

### 6.4 OOM 处理链

```
SystemAlloc 失败 → NewSpan 返回 nullptr
                 → GetOneSpan 返回 nullptr → FetchRangeObj 返回 0
                 → ThreadCache::FetchFromCentralCache 返回 nullptr → Allocate 返回 nullptr
                 → my_malloc 返回 nullptr
                 → operator new 抛 std::bad_alloc（nothrow 版返回 nullptr）
```

大对象路径 `my_malloc` 亦对 `NewSpan` 结果判空。`realloc` 失败时保留原块（符合语义），`calloc` 溢出时返回 nullptr。

---

## 7. 对外接口

### 7.1 与 malloc 同语义（memory_pool.hpp / memory_pool.cpp）

| 接口 | 语义要点 |
|---|---|
| `my_malloc(size)` | `size==0` 按 1 处理；`> MAX_BYTES` 走大对象页路径 |
| `my_free(ptr)` | `nullptr` 安全；反查不到 Span（外来指针）则安全空操作 |
| `my_calloc(num,size)` | 乘法溢出检查；分配后清零请求量 |
| `my_realloc(ptr,size)` | `ptr==nullptr` 等价 malloc；`size==0` 释放返回空；**同档原地复用**；跨档新分配+拷贝+释放 |
| `my_usable_size(ptr)` | 小对象返回定长规格，大对象返回整块页大小；外来指针返回 0 |

### 7.2 全局 operator new/delete（operator_new.cpp）

替换范围（C++ `new/delete` 表达式）：

```
operator new / new[]                    operator delete / delete[]  (含 sized)
operator new / new[](nothrow)           operator delete / delete[](nothrow)
```

C++17 起追加**带对齐全家桶**（`#if defined(__cpp_aligned_new)`）：

```
operator new / new[](size_t, align_val_t)                 operator delete / delete[](void*, align_val_t)
operator new / new[](size_t, align_val_t, nothrow_t)      operator delete / delete[](void*, size_t, align_val_t)
                                                          operator delete / delete[](void*, align_val_t, nothrow_t)
```

**过对齐实现**：池内只保证 16B / 整页对齐，故 `AlignedAlloc` 采用「多申请 `align + sizeof(void*)` → 返回对齐地址 → 在对齐地址前一个指针槽保存原始地址」；`AlignedFree` 由该槽还原原始指针再交回内存池。含 `size + align + 8` 溢出保护。

**语义边界**：

- 只重载 C++ `new/delete`，**不拦截 C 的 `malloc/free` 符号**（跨平台 CRT 符号差异大、易与动态库内部堆冲突）。
- MinGW 下 libstdc++ 是 DLL，其内部堆不受本重载影响；这类外来指针反查不到 Span 时 `my_free` 安全空操作（不崩溃、不回收）。
- 非对齐路径返回块最少 16B 对齐（最小规格 16、页起始 8KB 对齐），满足 `max_align_t`。

---

## 8. 构建与测试

### 8.1 构建

| 方式 | 命令 | 产物 |
|---|---|---|
| PowerShell（默认） | `./build.ps1` | `build/`，`-O2` / C++14 |
| PowerShell（sanitizer） | `./build.ps1 -San` | `build-san/`，`-fsanitize=address,undefined -g -O1` |
| CMake（常规） | `cmake -S . -B build && cmake --build build` | `build/`，ctest 已注册全部测试 |
| CMake（ASan+UBSan） | `cmake -S . -B build -DENABLE_SANITIZERS=ON && cmake --build build` | asan+ubsan 构建 |
| CMake（TSan） | `cmake -S . -B build -DENABLE_TSAN=ON && cmake --build build` | thread-sanitizer 构建 |
| 运行测试 | `ctest --test-dir build --output-on-failure` | 逐项 PASS/FAIL + 退出码 |

> **工具链要求**：`-San` / `ENABLE_SANITIZERS=ON` / `ENABLE_TSAN=ON` 需要工具链自带 sanitizer 运行时（Linux/GCC、Clang 支持）。本机 MSYS2 ucrt64 的 g++ **缺少 libasan/libtsan/libubsan**，开启后会在链接阶段失败（`cannot find -lasan`）。`ENABLE_SANITIZERS` 与 `ENABLE_TSAN` **互斥**（ASan 与 TSan 不能同构建），CMake 会在同时开启时报错。
>
> **覆盖边界**：池内存来自 `mmap`/`VirtualAlloc`，绕过 ASan 分配器，故 **ASan 抓不到池内越界/释放后使用**，其价值集中在栈/全局与部分 UB；本项目的并发正确性主要由 **TSan** 验证。

### 8.1.1 持续集成（GitHub Actions）

`.github/workflows/ci.yml` 在 `push` / `pull_request` 上跑三个并行 job：

| job | 配置 | 目的 |
|---|---|---|
| release (gcc) | `Release` | 常规构建 + 全部 ctest |
| asan+ubsan (gcc) | `-DENABLE_SANITIZERS=ON` | 内存/未定义行为检查 |
| tsan (gcc) | `-DENABLE_TSAN=ON` | 并发数据竞争检查 |

TSan job 会先执行 `sudo sysctl -w vm.mmap_rnd_bits=28`，规避较新内核因高地址随机位导致的 `unexpected memory mapping` 启动失败。

### 8.2 测试清单

| 目标 | 验证点 |
|---|---|
| `test_size_class` | 对齐映射、Index/Classes 互逆、NumMoveSize/Page 边界、内部碎片率阈值 |
| `test_page_map` | 三级基数树存取、高地址路径、覆盖读 |
| `test_merge` | 相邻页合并回整块并复用，且**未触发新的 SystemAlloc** |
| `test_alloc_api` | calloc 清零、realloc 保数据/同档原地复用、usable_size、new/delete 走内存池 |
| `test_release_os` | 大量释放后**确实发生 SystemFree**，空闲量被压回水位以下 |
| `test_aligned_new`（C++17 编译） | `alignas(64/128/256)` 对象与数组、4096 对齐、nothrow 对齐版 |
| `test_sanity` | 多规格读写不越界、大对象、0/null 边界、批量归还 |
| `test_interleave` | 反复申请/释放交错 |
| `test_thread` | 多线程并发分配/释放 |
| `bench` | 8B/64B/256B/4KB 四档吞吐、与 malloc 对比、峰值 RSS |

### 8.3 基准参考

**方法**：16 线程；每档「先全部分配并写满，再全部释放」；内存池 `my_malloc/my_free` 与系统 `malloc/free` 采用**完全相同的线程数、迭代数与访问模式**，仅分配器不同，保证可比。每档迭代数按「目标活跃内存 ≈ 256MB」反推（8B 档封顶 30 万/线程）。

```bash
build/bench.exe 16 256 both          # 四档：内存池 vs malloc 吞吐
build/bench.exe 16 256 pool          # 隔离进程，取内存池峰值 RSS
build/bench.exe 16 256 malloc        # 隔离进程，取 malloc 峰值 RSS
```

| 规格 | 每线程迭代 | 活跃内存 | 内存池 (M ops/s) | malloc (M ops/s) | 相对倍率 |
|---|---|---|---|---|---|
| 8 B   | 300000 | 36.6 MB | 30.8 | 95.2 | 0.32× |
| 64 B  | 262144 | 256 MB  | 16.2 | 32.5 | 0.50× |
| 256 B | 65536  | 256 MB  | 15.5 | 16.6 | 0.94× |
| 4 KB  | 4096   | 256 MB  | 1.5  | 1.2  | 1.28× |

> 吞吐为 alloc+free 合计；机器/运行波动约 ±10%（已多次复跑，倍率趋势稳定）。
> 数据由 `bench_report.ps1` 生成，见 [bench-report.html](bench-report.html)（本行为该次运行快照）。

**峰值 RSS**（隔离进程单档复跑）：内存池 **277.3 MB** vs malloc **349.0 MB**。内存池更低：无 per-alloc 头部，规格按需紧致（glibc 最小 chunk 32B，64B 实际占到 80B），对象越小差距越明显。

**结论与取舍**：

- **大对象（≥4KB）**：内存池吞吐反超 malloc，活跃内存更紧凑。
- **极小对象（8B/64B）**：本教学实现落后于 glibc 的 `tcache`。差距来自每次分配/释放的固定开销——`my_malloc/my_free` 调用链、`RoundUp/Index` 规格计算、释放时经三级 PageMap 反查 Span（3 次依赖原子读）、以及 `ThreadCache::Get()` 的 `thread_local` 守卫。
- 本实现的优势区间是**内存占用与最坏情况延迟可控**（分桶 + 相邻页合并 + 整块归还 OS），而非与 glibc `tcache` 争夺极小对象的单次分配速度；若要追平，需引入 per-CPU 缓存、更大的线程缓存容量等 tcmalloc 高级特性（见 §10）。

> 注：以上为 MinGW-w64 / ucrt64、`-O2`、本机实测，仅代表当前实现与运行环境。

---

## 9. 复杂度与性能特性

| 操作 | 复杂度 | 是否加锁 |
|---|---|---|
| `ThreadCache::Allocate`（命中） | O(1) | 否 |
| `ThreadCache::Allocate`（跨层领取） | O(batch) | 档位锁 |
| `my_free` 小对象快路径 | O(1) | 否 |
| `my_free` 批量归还 | O(k) | 档位锁 + 页锁（每次反查 O(1)） |
| `NewSpan` | O(MAX_PAGES) 桶扫描（≤128，常数小）+ 必要时 SystemAlloc | 页锁 |
| `ReleaseSpanToPageCache` | O(合并的 Span 数) | 页锁 |
| `_tryReleaseToOS` | O(归还的整块数) | 页锁 |

设计取向：**把锁竞争压到最低频的路径**（跨层批量、Span 分配/回收、归还 OS），高频的分配/释放走无锁。

---

## 10. 已知限制与后续方向

1. **归还 OS 以整块为粒度**：受 `MAX_PAGES`（1MB）约束，只有完整空闲整块能归还；碎片化的小块空闲暂不归还（Linux 可用 `munmap` 子区间、Windows 需 `MEM_DECOMMIT` 进一步优化）。
2. **不拦截 C 的 `malloc/free`**：只替换 C++ `new/delete`；与动态库内部堆混用时外来指针安全但不回收。
3. **仅本机 MinGW 实测**：Linux 的 `mmap` 分支代码完整但未在 Linux 上跑过；sanitizer 开关需带 sanitizer 运行时的工具链。
4. **线程缓存析构顺序**：进程退出阶段若 `ThreadCache` 已析构又被 `operator delete` 触达，会写入生命周期已结束的对象（实际内存有效、仅少量不回收，不崩溃）。
5. **未包含**：per-CPU 缓存、背景回收线程、采样/统计、跨进程共享等 tcmalloc 高级特性。

---

## 11. 文件地图

| 文件 | 职责 |
|---|---|
| `include/common.hpp` | 常量、地址换算、SystemAlloc/Free、计数器、Immortal 单例 |
| `include/size_class.hpp` | 字节→定长规格映射、move size/page |
| `include/freelist.hpp` | 侵入式自由链表 |
| `include/span.hpp` | Span、SpanList |
| `include/page_map.hpp` | 三级基数树（无锁读） |
| `include/object_pool.hpp` | 定长对象池（Span 的对象来源） |
| `include/thread_cache.hpp` | 线程缓存（TLS、慢启动、批量取还） |
| `include/central_cache.hpp` / `src/central_cache.cpp` | 中心缓存（每档锁、Span 切分/回收） |
| `include/page_cache.hpp` / `src/page_cache.cpp` | 页缓存（分桶、拆分、合并、归还 OS） |
| `include/memory_pool.hpp` / `src/memory_pool.cpp` | my_malloc/free/calloc/realloc/usable_size |
| `src/operator_new.cpp` | 全局 operator new/delete（含 C++17 对齐） |
| `tests/`、`benchmark/` | 测试与基准 |
| `build.ps1`、`CMakeLists.txt` | 构建脚本与工程配置 |
