/* PlayShell: included first into every file of an emulator core built for the PS5 (tools/*_lib.sh, -include).
 * The PS5 payload SDK's libc is FreeBSD's: what Linux-minded code expects differently is settled here. */
#pragma once
#ifndef __ASSEMBLER__ /* (assembly files get it too: nothing for them) */
#include <time.h>
#include <pthread.h>
#include <pthread_np.h>
#include <sys/param.h>
#include <sys/mman.h>

/* FreeBSD's <sys/param.h> macros clash with functions of the same name (PPSSPP's MemArena::roundup) */
#undef roundup
#undef rounddown

/* Linux's thread naming (FreeBSD: pthread_set_name_np, returns nothing) */
#ifndef pthread_setname_np
#define pthread_setname_np(t, n) (pthread_set_name_np((t), (n)), 0)
#endif

/* No X11 on the PS5: Vulkan's Xlib part stays out (PPSSPP draws in software here) */
#undef VK_USE_PLATFORM_XLIB_KHR

#ifdef __cplusplus
extern "C" {
#endif
/* Linux's auxiliary vector (CPU features on ARM; x86 asks the CPU itself): nothing here (a core with its own
 * stand-in - Crypto++ - builds with PS5_COMPAT_NO_GETAUXVAL) */
#ifndef PS5_COMPAT_NO_GETAUXVAL
static inline unsigned long getauxval(unsigned long type) {
	(void)type;
	return 0;
}
#endif
#ifdef __cplusplus
}
#endif
#endif /* __ASSEMBLER__ */
