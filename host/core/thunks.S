/* Common tail of every missing-import thunk ("push index; jmp gmv_missing_entry").
   The stack already holds the import index and the game's return address, which are
   exactly the two cdecl arguments of gmv_missing_import. */
#ifdef _WIN32
#define SYM(x) _##x
#else
#define SYM(x) x
#endif
    .text
    .globl SYM(gmv_missing_entry)
SYM(gmv_missing_entry):
    call SYM(gmv_missing_import)
    ud2
#ifndef _WIN32
    .section .note.GNU-stack,"",@progbits
#endif
