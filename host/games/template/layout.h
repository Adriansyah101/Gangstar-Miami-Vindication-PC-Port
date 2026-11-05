/* What the host needs to know about this game; copy this folder to games\<yourgame> and fill it in.  Shared by C and
   the .S reservation.  tools\genlow.py makes the low-range patch table for the binary.

   The game must be a non-PIE i386 Mach-O: it runs at its own addresses.  The exe is based at 0x10000 and its first
   section covers the game's range, so nothing else can be mapped there.  The code below LOW_END runs from a copy at
   LOWCOPY_BASE. */
#ifndef GMV_LAYOUT_H
#define GMV_LAYOUT_H

#define GMV_EXE_BASE        0x00010000
#define GMV_RESERVE_START   0x00011000      /* first byte the exe reserves for the game */
#define GMV_LOW_START       0x00001000      /* the copied range starts at the Mach-O header */

/* From the binary's load commands: the first value is the end of __LINKEDIT, the last segment, and everything after
   it must stay above it. */
#define GMV_MACHO_END       0x00C68000
#define GMV_LOWCOPY_BASE    0x00C70000      /* copy of game bytes [LOW_START, low_end) */
#define GMV_LOWCOPY_CAP     0x00020000      /* low_end comes from gmv_patch.bin */
#define GMV_THUNK_BASE      0x00C90000      /* host-generated import thunks (code) */
#define GMV_THUNK_SIZE      0x00008000
#define GMV_DATAGUARD_BASE  0x00C98000      /* no-access stand-ins for missing data imports */
#define GMV_DATAGUARD_SIZE  0x00008000
#define GMV_RESERVE_END     0x00D00000

#define GMV_LOW_DELTA       (GMV_LOWCOPY_BASE - GMV_LOW_START)

/* The game's names, in what the host writes and looks for. */
#define GMV_GAME_NAME   "Template"                      /* the exe, the bundle and the binary in it, .ini, .log, .dmp */
#define GMV_GAME_TITLE  "Template"                      /* the title of the window and the game's alerts */
#define GMV_GAME_DEV_APP "Template.app"                 /* a dev build also looks here */

/* The Objective-C classes the host makes or watches for. */
#define GMV_CLASS_APP       "GameApplication"
#define GMV_CLASS_DELEGATE  "GameDelegate"
#define GMV_CLASS_VIEW      "EAGLView"
#define GMV_CLASS_WINDOW    "GameWindow"
/* The view's frame in the game's own units, which mouse events are given in. */
#define GMV_VIEW_FRAME_W    1024.0f
#define GMV_VIEW_FRAME_H    768.0f
/* Addresses in the game that the shims need (0 = this game has none):
   a byte the window's key notification sets, which the game checks before taking mouse events;
   the import slot holding std::basic_stringbuf<char>'s vtable. */
#define GMV_KEYWINDOW_FLAG_ADDR     0u
#define GMV_STRINGBUF_VTABLE_SLOT   0u

/* The game's __text, for the diagnostics' stack scans. */
#define GMV_TEXT_LO     0x00003000u
#define GMV_TEXT_HI     0x00600000u

#endif
