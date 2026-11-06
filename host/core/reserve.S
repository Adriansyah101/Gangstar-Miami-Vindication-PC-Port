/* reserve.S -- Windows: the game's address range as an uninitialised section of the exe itself: it costs no
   file space, and the kernel reserves it when it maps the exe (see gmv.ld, layout.h).
   Linux maps the range when it starts (platform_linux.c). */
#ifdef _WIN32
#include "layout.h"

    .section .gmvimg,"bwx"
    .globl _gmv_reserve_start
    .globl _gmv_reserve_end
_gmv_reserve_start:
    .space GMV_RESERVE_END - GMV_RESERVE_START
_gmv_reserve_end:
#else
    .section .note.GNU-stack,"",@progbits
#endif
