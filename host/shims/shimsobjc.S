/* objc_msgSend for the game's legacy i386 Objective-C.

   id objc_msgSend(id self, SEL _cmd, ...) -- the receiver and selector are the first two stack
   arguments.  We ask the C runtime for the method implementation and TAIL-JUMP to it with the
   stack untouched, so the IMP sees the original arguments and returns straight to our caller
   (exactly how the real objc_msgSend works).  A zero IMP means "the result is nil" (e.g. a
   message to nil, or a no-op like -release); we just return 0.  gmv_objc_lookup never returns
   for a message it cannot handle -- it logs and exits.

   Linux: C symbols have no leading underscore, and C code there expects the stack 16-byte aligned
   at each call, so ALIGN_CALL(n) makes room before n bytes of pushed arguments (the game's own
   call left esp 4 below a boundary on entry).  UNALIGN(n) takes it back with the arguments.
   Windows keeps the bytes it always had. */
#ifdef _WIN32
#define SYM(x) _##x
#define ALIGN_CALL(n)
#define POP_ARGS(n) addl $n, %esp
#else
#define SYM(x) x
#define ALIGN_CALL(n) subl $(12 - (n)) & 15, %esp
#define POP_ARGS(n) addl $(n) + ((12 - (n)) & 15), %esp
#endif
    .text
    .globl SYM(gmv_objc_msgSend)
SYM(gmv_objc_msgSend):
    movl    4(%esp), %eax        /* self */
    movl    8(%esp), %edx        /* _cmd (SEL = char* selector name) */
    ALIGN_CALL(8)
    pushl   %edx
    pushl   %eax
    call    SYM(gmv_objc_lookup)     /* -> IMP in %eax, or 0 */
    POP_ARGS(8)
    testl   %eax, %eax
    jz      1f
    jmp     *%eax                /* tail-call the IMP; original args are still in place */
1:
    xorl    %eax, %eax           /* nil result */
    ret

/* double/float objc_msgSend_fpret(id self, SEL _cmd, ...) -- the i386 variant for methods that
   return a floating-point value (e.g. -[NSEvent deltaY], a CGFloat).  Identical dispatch to
   objc_msgSend: look up the IMP and tail-jump, so the IMP's ST(0) return reaches our caller.  A nil
   receiver yields 0.0 (ST(0)) rather than an integer 0. */
    .globl SYM(gmv_objc_msgSend_fpret)
SYM(gmv_objc_msgSend_fpret):
    movl    4(%esp), %eax        /* self */
    movl    8(%esp), %edx        /* _cmd */
    ALIGN_CALL(8)
    pushl   %edx
    pushl   %eax
    call    SYM(gmv_objc_lookup)     /* -> IMP in %eax, or 0 */
    POP_ARGS(8)
    testl   %eax, %eax
    jz      3f
    jmp     *%eax                /* tail-call the IMP; float/double result stays in ST(0) */
3:
    fldz                         /* nil receiver -> 0.0 */
    ret

/* id objc_msgSendSuper(struct objc_super *super, SEL _cmd, ...) -- super = {receiver, class}.
   The method is looked up starting from super->class; the IMP is then called with the real
   receiver (super->receiver) as arg0, so we overwrite the struct pointer on the stack with it. */
    .globl SYM(gmv_objc_msgSendSuper)
SYM(gmv_objc_msgSendSuper):
    movl    4(%esp), %eax        /* struct objc_super* */
    movl    8(%esp), %edx        /* _cmd */
    ALIGN_CALL(8)
    pushl   %edx
    pushl   %eax
    call    SYM(gmv_objc_super_lookup)   /* -> IMP in %eax, or 0 */
    POP_ARGS(8)
    testl   %eax, %eax
    jz      2f
    movl    4(%esp), %ecx        /* struct objc_super* */
    movl    (%ecx), %ecx         /* super->receiver */
    movl    %ecx, 4(%esp)        /* arg0 = receiver */
    jmp     *%eax
2:
    xorl    %eax, %eax
    ret

/* void objc_msgSend_stret(void *sret, id self, SEL _cmd, ...) -- for methods returning a struct
   by value.  The hidden struct-return pointer is arg0, then self, then _cmd.  We look up by
   (self,_cmd) and tail-jump to the IMP with the stack intact, so the IMP sees {sret, self, sel,
   ...} and (on macOS i386) pops the sret pointer itself (ret $4). */
    .globl SYM(gmv_objc_msgSend_stret)
SYM(gmv_objc_msgSend_stret):
    movl    8(%esp), %eax        /* self */
    movl    12(%esp), %edx       /* _cmd */
    ALIGN_CALL(8)
    pushl   %edx
    pushl   %eax
    call    SYM(gmv_objc_stret_lookup)   /* -> IMP in %eax, or 0 */
    POP_ARGS(8)
    testl   %eax, %eax
    jz      4f
    jmp     *%eax
4:
    ret

/* gmv_gl_forward -- generic cdecl->stdcall adapter for opengl32.  Entered with %eax = the real
   stdcall GL function; [esp] = the game's return address, [esp+4..] = its cdecl args.  We copy a
   fixed 64 bytes of args into a fresh frame and call the GL function (which pops its own args via
   `ret N`), then restore esp from ebp (so N need not be known) and return cdecl (the game cleans
   its args).  Callee-saved esi/edi/ebx/ebp are preserved; GL return value stays in eax. */
#ifdef _WIN32
    .globl _gmv_gl_forward
_gmv_gl_forward:
    pushl   %ebp
    movl    %esp, %ebp
    pushl   %esi
    pushl   %edi
    pushl   %ebx
    movl    %eax, %ebx           /* ebx = real GL function */
    subl    $64, %esp
    leal    8(%ebp), %esi        /* src = the game's args */
    movl    %esp, %edi           /* dst = scratch */
    movl    $16, %ecx
    rep     movsl                /* copy 64 bytes (16 dwords) */
    call    *%ebx                /* stdcall GL fn cleans its own args */
    leal    -12(%ebp), %esp      /* restore past the 3 saved regs regardless of how much it popped */
    popl    %ebx
    popl    %edi
    popl    %esi
    popl    %ebp
    ret
#endif

/* gmv_gl_lazy -- entry for a lazily-bound GL thunk.  %eax = a glcell {void *fn; char *name;}.
   Resolve fn on first use (gl_real), then forward via gmv_gl_forward.
   An unresolved function returns 0 (the game continues). */
    .globl SYM(gmv_gl_lazy)
SYM(gmv_gl_lazy):
    movl    (%eax), %edx         /* cell->fn */
    testl   %edx, %edx
    jnz     5f
    ALIGN_CALL(4)
    pushl   %eax                 /* cell */
    call    SYM(gmv_gl_lookup)   /* -> fn in %eax (also cached) */
#ifdef _WIN32
    popl    %ecx
#else
    POP_ARGS(4)
#endif
    movl    %eax, %edx
5:
    testl   %edx, %edx
    jz      6f
#ifdef _WIN32
    movl    %edx, %eax
    jmp     _gmv_gl_forward
#else
    jmp     *%edx                /* GL is cdecl there too: its arguments are the game's, as they stand */
#endif
6:
    xorl    %eax, %eax
    ret

/* std::basic_ostringstream/stringstream::str() const -- returns a std::string BY VALUE.  Because
   std::string has a non-trivial (COW) copy ctor, macOS i386 returns it through a hidden struct-
   return pointer the CALLEE pops (ret $4), like objc_msgSend_stret.  Stack in: [esp]=return,
   [esp+4]=sret (the std::string to construct), [esp+8]=self (the stream).  oss_str_impl(self)
   builds the COW string and returns its _M_p; the std::string object is exactly that one pointer,
   so we store it into *sret, return sret in eax, and ret $4 to pop the hidden pointer. */
    .globl SYM(gmv_oss_str)
SYM(gmv_oss_str):
    movl    8(%esp), %eax        /* self (the stream) */
    ALIGN_CALL(4)
    pushl   %eax
    call    SYM(oss_str_impl)    /* -> _M_p in %eax */
    POP_ARGS(4)
    movl    4(%esp), %edx        /* sret (the std::string result slot) */
    movl    %eax, (%edx)         /* *sret = _M_p */
    movl    %edx, %eax           /* return the sret pointer */
    ret     $4                   /* pop the hidden struct-return pointer (Darwin callee-pop) */

/* setjmp/longjmp with our own jmp_buf layout (the game only ever pairs our setjmp with our
   longjmp; its buffer is a macOS jmp_buf, far larger than the 24 bytes we use).  Saves the
   callee-saved regs, the post-return esp, and the return address. */
    .globl SYM(gmv_setjmp)
SYM(gmv_setjmp):
    movl    4(%esp), %eax
    movl    %ebx, 0(%eax)
    movl    %esi, 4(%eax)
    movl    %edi, 8(%eax)
    movl    %ebp, 12(%eax)
    leal    4(%esp), %ecx        /* esp as it will be after ret pops the return address */
    movl    %ecx, 16(%eax)
    movl    (%esp), %ecx         /* return address */
    movl    %ecx, 20(%eax)
    xorl    %eax, %eax
    ret

    .globl SYM(gmv_longjmp)
SYM(gmv_longjmp):
    movl    4(%esp), %edx        /* jmp_buf */
    movl    8(%esp), %eax        /* value */
    testl   %eax, %eax
    jnz     7f
    incl    %eax                 /* longjmp(buf,0) must appear as 1 */
7:
    movl    0(%edx), %ebx
    movl    4(%edx), %esi
    movl    8(%edx), %edi
    movl    12(%edx), %ebp
    movl    16(%edx), %esp
    movl    20(%edx), %ecx
    jmp     *%ecx                /* resume at setjmp's return site, eax = value */
#ifndef _WIN32
    .section .note.GNU-stack,"",@progbits
#endif
