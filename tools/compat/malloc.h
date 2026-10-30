/* PlayShell: Linux's <malloc.h> for emulator cores built for the PS5 (FreeBSD's libc refuses it: use <stdlib.h>). */
#pragma once
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
void*  memalign(size_t alignment, size_t size);
size_t malloc_usable_size(void* ptr);
#ifdef __cplusplus
}
#endif
