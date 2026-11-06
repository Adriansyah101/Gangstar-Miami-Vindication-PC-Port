/* layout.h -- what the host needs to know about this game: the fixed address-space layout of GangstarMV.exe,
   shared by C and the .S reservation, and the game's names.  Linux keeps the same layout, mapped when the program
   starts (platform_linux.c), so gmv_patch.bin is the same there; GMV_EXE_BASE is Windows' only.

   The game is a non-PIE i386 Mach-O linked at 0x1000 and has no rebase info, so it runs at
   its own addresses.  The exe is based at 0x10000 (the lowest address Windows allows) and
   its first section covers the game's range, so nothing else can be mapped there.  Only
   the code below LOW_END cannot sit at its own address: the PE header page owns
   0x10000..0x11000 and nothing at all may be mapped below 0x10000.  That code runs from a
   copy at LOWCOPY_BASE, with every branch into or out of it patched exactly (the patch
   list is tools/genlow.py's gmv_patch.bin); a stray jump into the range faults and the
   exception handler moves it into the copy. */
#ifndef GMV_LAYOUT_H
#define GMV_LAYOUT_H

#define GMV_EXE_BASE        0x00010000
#define GMV_RESERVE_START   0x00011000      /* first byte the exe reserves for the game */
#define GMV_LOW_START       0x00001000      /* the copied range starts at the Mach-O header */

#define GMV_MACHO_END       0x00C68000      /* end of __LINKEDIT, the last segment */
#define GMV_LOWCOPY_BASE    0x00C70000      /* copy of game bytes [LOW_START, low_end) */
#define GMV_LOWCOPY_CAP     0x00020000      /* low_end comes from gmv_patch.bin */
#define GMV_THUNK_BASE      0x00C90000      /* host-generated import thunks (code) */
#define GMV_THUNK_SIZE      0x00008000
#define GMV_DATAGUARD_BASE  0x00C98000      /* no-access stand-ins for missing data imports */
#define GMV_DATAGUARD_SIZE  0x00008000
#define GMV_RESERVE_END     0x00D00000

#define GMV_LOW_DELTA       (GMV_LOWCOPY_BASE - GMV_LOW_START)

/* The game's names, in what the host writes and looks for. */
#define GMV_GAME_NAME   "GangstarMV"                    /* the exe, the bundle and the binary in it, .ini, .log, .dmp */
#define GMV_GAME_TITLE  "Gangstar: Miami Vindication"   /* the title of the game's alerts */
#define GMV_GAME_DEV_APP "../../extracted/Gangstar Miami Vindication/GangstarMV.app"     /* a dev build also looks here */

/* The Objective-C classes the host makes or watches for. */
#define GMV_CLASS_APP       "GameApplication"
#define GMV_CLASS_DELEGATE  "GameDelegate"
#define GMV_CLASS_VIEW      "EAGLView"
#define GMV_CLASS_WINDOW    "GameWindow"
/* The view's frame in the game's own units, which mouse events are given in. */
#define GMV_VIEW_FRAME_W    1024.0f
#define GMV_VIEW_FRAME_H    768.0f
/* Addresses in the game that the shims need (0 = this game has none):
   the byte -[GameWindow windowDidBecomeKey:] sets, from `mov byte [pic+0x4ef670], 1` at 0x19b03c;
   the import slot holding std::basic_stringbuf<char>'s vtable. */
#define GMV_KEYWINDOW_FLAG_ADDR     0x0068a6abu
#define GMV_STRINGBUF_VTABLE_SLOT   0x0063d3ecu

/* The game's __text, for the diagnostics' stack scans. */
#define GMV_TEXT_LO     0x00003170u
#define GMV_TEXT_HI     0x005ed157u

#endif
