#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void *heap_caps_aligned_alloc(size_t align, size_t size, int caps)
{
    (void)caps;
    void *ptr = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    return posix_memalign(&ptr, align, size) == 0 ? ptr : NULL;
}
