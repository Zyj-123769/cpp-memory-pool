#pragma once
#include <cstddef>
#include "thread_cache.hpp"
#include "page_cache.hpp"

namespace tcm {

// 全局分配入口：与 malloc/free 同签名
void* my_malloc(size_t size);
void  my_free(void* ptr);

// 与 calloc/realloc/malloc_usable_size 同语义的配套接口
void*  my_calloc(size_t num, size_t size);
void*  my_realloc(void* ptr, size_t size);
size_t my_usable_size(void* ptr);

} // namespace tcm