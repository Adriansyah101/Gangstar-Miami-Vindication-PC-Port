/* shimscxx.c -- the C++ runtime and libstdc++ entry points the game's static initializers
   reach (per tools/initreach.py).  Everything here is plain C against the Itanium i386 ABI the
   game was built with (arguments on the stack, caller cleans, `this` is the first argument);
   the Mach-O symbol strings are the GCC-mangled names.

   Scope is deliberately the init-reachable set only:
     - the C++ ABI support calls (__cxa_atexit, __cxa_guard_*, __cxa_pure_virtual);
     - std::ios_base::Init::Init(), a no-op -- see the note on it below;
     - the std::__throw_* reporters, which only run on an allocation/overflow error path;
     - the red-black tree helpers (increment/decrement/insert), ported verbatim from the
       libstdc++ 4.2 tree.cc so std::map / std::set globals can be populated during init.
   std::string, the stream classes, and locale are NOT here; the game uses them from main(),
   not from a static initializer, so they belong to a later (gameplay) stage. */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600             /* SRWLOCK and CONDITION_VARIABLE */
#endif

#include <stdint.h>
#include <stdlib.h>
#include "compat.h"

#include "log.h"
#include "shims.h"

#define FN(mach_name, host_fn) {mach_name, (void *)(host_fn)}

/* ---- C++ ABI support ------------------------------------------------------------------ */

/* The static-initializer phase never runs destructors, so registration can simply succeed. */
static int shim_cxa_atexit(void (*func)(void *), void *arg, void *dso)
{
    (void)func;
    (void)arg;
    (void)dso;
    return 0;
}

/* Guard variables of function-local statics: 8 bytes on i386, byte 0 = initialised (the game's
   inlined fast path tests only that byte), byte 1 = being initialised.  Function-local statics
   also start up during play, on the loader and audio threads too, so two threads can reach the
   same first use; the Mac's runtime makes the second wait for the first, and so does this
   (libc++abi's scheme: one lock and a condition variable, the lock not held while the static's
   constructor runs).  A thread waiting on a static it is itself constructing would wait forever,
   as it would on the Mac. */
static SRWLOCK g_guard_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_guard_cv = CONDITION_VARIABLE_INIT;

static int shim_cxa_guard_acquire(void *guard)
{
    volatile unsigned char *g = guard;
    if (g[0])
        return 0;
    AcquireSRWLockExclusive(&g_guard_lock);
    while (g[1])                                /* another thread is constructing it */
        SleepConditionVariableSRW(&g_guard_cv, &g_guard_lock, INFINITE, 0);
    int todo = !g[0];
    if (todo)
        g[1] = 1;
    ReleaseSRWLockExclusive(&g_guard_lock);
    return todo;
}

static void shim_cxa_guard_release(void *guard)
{
    volatile unsigned char *g = guard;
    AcquireSRWLockExclusive(&g_guard_lock);
    g[0] = 1;
    g[1] = 0;
    ReleaseSRWLockExclusive(&g_guard_lock);
    WakeAllConditionVariable(&g_guard_cv);
}

static void shim_cxa_guard_abort(void *guard)
{
    volatile unsigned char *g = guard;
    AcquireSRWLockExclusive(&g_guard_lock);
    g[1] = 0;
    ReleaseSRWLockExclusive(&g_guard_lock);
    WakeAllConditionVariable(&g_guard_cv);
}

static void shim_cxx_fatal(const char *what)
{
    gmv_log("\n[shim] C++ runtime: %s -- stopping\n", what);
    gmv_fatal(5, "The game stopped on a C++ runtime error (%s).", what);
}

static void shim_pure_virtual(void) { shim_cxx_fatal("pure virtual function called"); }

/* std::allocator<char> is a stateless empty class; its ctor/copy-ctor/dtor do nothing.  (Takes
   `this`, plus a source ref for the copy ctor, which we ignore.) */
static void shim_alloc_noop(void *self) { (void)self; }

/* ---- std::ios_base::Init::Init() ------------------------------------------------------ */

/* Each translation unit that includes <iostream> emits a static `ios_base::Init`, so 208 of
   the initializers call this.  The object is empty (size 1) and carries no state of its own;
   in real libstdc++ the first call wires up std::cout/cin/cerr.  We make it a no-op: none of
   the initializers use the standard streams (initreach confirms), so they stay unconstructed
   until a later stage builds real stream support.  Takes only the `this` pointer. */
static void shim_ios_base_init(void *self) { (void)self; }

/* ---- std::__throw_* (error-path reporters) -------------------------------------------- */

static void shim_throw_bad_alloc(void)             { shim_cxx_fatal("std::bad_alloc"); }
static void shim_throw_length_error(const char *m) { gmv_log("[shim] std::length_error: %s\n", m ? m : ""); shim_cxx_fatal("length_error"); }
static void shim_throw_logic_error(const char *m)  { gmv_log("[shim] std::logic_error: %s\n", m ? m : ""); shim_cxx_fatal("logic_error"); }
static void shim_throw_out_of_range(const char *m) { gmv_log("[shim] std::out_of_range: %s\n", m ? m : ""); shim_cxx_fatal("out_of_range"); }

/* for the other C++ shims (shimsstr.c) */
void gmv_throw_out_of_range(const char *m) { shim_throw_out_of_range(m); }
void gmv_throw_length_error(const char *m) { shim_throw_length_error(m); }
void gmv_throw_bad_alloc(void)             { shim_throw_bad_alloc(); }

/* ---- nothrow operator new / delete ---------------------------------------------------- */

/* The game statically links libstdc++, so the throwing operators new/new[]/delete/delete[]
   (_Znwm/_Znam/_ZdlPv/_ZdaPv) resolve to its own malloc/free-backed definitions (weak binds).
   The NOTHROW variants -- operator new(size_t, nothrow_t const&) and the array/delete forms --
   are not emitted in the binary, so they arrive here as real imports.  libstdc++ 4.2's nothrow
   new is just "try the throwing new; return 0 on bad_alloc", and its delete is plain free, so
   malloc/free here share the one heap with the game's own operators and stay delete-compatible.
   The second argument is &std::nothrow (the empty tag object); we ignore it.  i386 cdecl, caller
   cleans -- matches the Itanium ABI the throwing operators use. */
static void *shim_new_nothrow(size_t sz, const void *tag) { (void)tag; return malloc(sz ? sz : 1); }
static void  shim_delete_nothrow(void *p, const void *tag) { (void)tag; free(p); }

/* ---- red-black tree helpers (libstdc++ 4.2 tree.cc) ----------------------------------- */

/* _Rb_tree_node_base: { _Rb_tree_color _M_color; _Base_ptr _M_parent, _M_left, _M_right; }.
   On i386 the colour enum is a 4-byte int, so the node base is 16 bytes. */
#define RB_RED   0
#define RB_BLACK 1

typedef struct rbnode {
    int color;
    struct rbnode *parent;
    struct rbnode *left;
    struct rbnode *right;
} rbnode;

static rbnode *rb_increment(rbnode *x)
{
    if (x->right) {
        x = x->right;
        while (x->left)
            x = x->left;
    } else {
        rbnode *y = x->parent;
        while (x == y->right) {
            x = y;
            y = y->parent;
        }
        if (x->right != y)
            x = y;
    }
    return x;
}

static rbnode *rb_decrement(rbnode *x)
{
    if (x->color == RB_RED && x->parent->parent == x) {   /* x is the header */
        x = x->right;
    } else if (x->left) {
        rbnode *y = x->left;
        while (y->right)
            y = y->right;
        x = y;
    } else {
        rbnode *y = x->parent;
        while (x == y->left) {
            x = y;
            y = y->parent;
        }
        x = y;
    }
    return x;
}

static void rb_rotate_left(rbnode *x, rbnode **root)
{
    rbnode *y = x->right;
    x->right = y->left;
    if (y->left)
        y->left->parent = x;
    y->parent = x->parent;
    if (x == *root)
        *root = y;
    else if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void rb_rotate_right(rbnode *x, rbnode **root)
{
    rbnode *y = x->left;
    x->left = y->right;
    if (y->right)
        y->right->parent = x;
    y->parent = x->parent;
    if (x == *root)
        *root = y;
    else if (x == x->parent->right)
        x->parent->right = y;
    else
        x->parent->left = y;
    y->right = x;
    x->parent = y;
}

/* insert_left is a C++ bool, passed as a 4-byte stack slot; header& is passed as a pointer. */
static void rb_insert_and_rebalance(int insert_left, rbnode *x, rbnode *p, rbnode *header)
{
    rbnode **root = &header->parent;

    x->parent = p;
    x->left = NULL;
    x->right = NULL;
    x->color = RB_RED;

    if (insert_left) {
        p->left = x;                    /* also makes leftmost = x when p == header */
        if (p == header) {
            header->parent = x;
            header->right = x;
        } else if (p == header->left) {
            header->left = x;
        }
    } else {
        p->right = x;
        if (p == header->right)
            header->right = x;
    }

    while (x != *root && x->parent->color == RB_RED) {
        rbnode *xpp = x->parent->parent;
        if (x->parent == xpp->left) {
            rbnode *y = xpp->right;
            if (y && y->color == RB_RED) {
                x->parent->color = RB_BLACK;
                y->color = RB_BLACK;
                xpp->color = RB_RED;
                x = xpp;
            } else {
                if (x == x->parent->right) {
                    x = x->parent;
                    rb_rotate_left(x, root);
                }
                x->parent->color = RB_BLACK;
                xpp->color = RB_RED;
                rb_rotate_right(xpp, root);
            }
        } else {
            rbnode *y = xpp->left;
            if (y && y->color == RB_RED) {
                x->parent->color = RB_BLACK;
                y->color = RB_BLACK;
                xpp->color = RB_RED;
                x = xpp;
            } else {
                if (x == x->parent->left) {
                    x = x->parent;
                    rb_rotate_right(x, root);
                }
                x->parent->color = RB_BLACK;
                xpp->color = RB_RED;
                rb_rotate_left(xpp, root);
            }
        }
    }
    (*root)->color = RB_BLACK;
}

/* _Rb_tree_rebalance_for_erase (libstdc++ 4.2 tree.cc) -- remove z, return the node to free. */
static rbnode *rb_rebalance_for_erase(rbnode *z, rbnode *header)
{
    rbnode **root = &header->parent, **leftmost = &header->left, **rightmost = &header->right;
    rbnode *y = z, *x = NULL, *x_parent = NULL;

    if (!y->left)
        x = y->right;
    else if (!y->right)
        x = y->left;
    else {
        y = y->right;
        while (y->left)
            y = y->left;
        x = y->right;
    }
    if (y != z) {
        z->left->parent = y;
        y->left = z->left;
        if (y != z->right) {
            x_parent = y->parent;
            if (x)
                x->parent = y->parent;
            y->parent->left = x;
            y->right = z->right;
            z->right->parent = y;
        } else {
            x_parent = y;
        }
        if (*root == z)
            *root = y;
        else if (z->parent->left == z)
            z->parent->left = y;
        else
            z->parent->right = y;
        y->parent = z->parent;
        int tc = y->color; y->color = z->color; z->color = tc;
        y = z;
    } else {
        x_parent = y->parent;
        if (x)
            x->parent = y->parent;
        if (*root == z)
            *root = x;
        else if (z->parent->left == z)
            z->parent->left = x;
        else
            z->parent->right = x;
        if (*leftmost == z) {
            if (!z->right) {
                *leftmost = z->parent;
            } else {
                rbnode *m = x;
                while (m->left) m = m->left;
                *leftmost = m;
            }
        }
        if (*rightmost == z) {
            if (!z->left) {
                *rightmost = z->parent;
            } else {
                rbnode *m = x;
                while (m->right) m = m->right;
                *rightmost = m;
            }
        }
    }
    if (y->color != RB_RED) {
        while (x != *root && (x == NULL || x->color == RB_BLACK)) {
            if (x == x_parent->left) {
                rbnode *w = x_parent->right;
                if (w->color == RB_RED) {
                    w->color = RB_BLACK; x_parent->color = RB_RED;
                    rb_rotate_left(x_parent, root); w = x_parent->right;
                }
                if ((!w->left || w->left->color == RB_BLACK) &&
                    (!w->right || w->right->color == RB_BLACK)) {
                    w->color = RB_RED; x = x_parent; x_parent = x_parent->parent;
                } else {
                    if (!w->right || w->right->color == RB_BLACK) {
                        if (w->left) w->left->color = RB_BLACK;
                        w->color = RB_RED; rb_rotate_right(w, root); w = x_parent->right;
                    }
                    w->color = x_parent->color; x_parent->color = RB_BLACK;
                    if (w->right) w->right->color = RB_BLACK;
                    rb_rotate_left(x_parent, root); break;
                }
            } else {
                rbnode *w = x_parent->left;
                if (w->color == RB_RED) {
                    w->color = RB_BLACK; x_parent->color = RB_RED;
                    rb_rotate_right(x_parent, root); w = x_parent->left;
                }
                if ((!w->right || w->right->color == RB_BLACK) &&
                    (!w->left || w->left->color == RB_BLACK)) {
                    w->color = RB_RED; x = x_parent; x_parent = x_parent->parent;
                } else {
                    if (!w->left || w->left->color == RB_BLACK) {
                        if (w->right) w->right->color = RB_BLACK;
                        w->color = RB_RED; rb_rotate_left(w, root); w = x_parent->left;
                    }
                    w->color = x_parent->color; x_parent->color = RB_BLACK;
                    if (w->left) w->left->color = RB_BLACK;
                    rb_rotate_right(x_parent, root); break;
                }
            }
        }
        if (x)
            x->color = RB_BLACK;
    }
    return y;
}

/* ---- std::list node base (libstdc++ 4.2 list.cc; node = {next@0, prev@4}) ------------- */

typedef struct lnode { struct lnode *next, *prev; } lnode;

static void list_hook(lnode *self, lnode *pos)   /* insert self before pos */
{
    self->next = pos;
    self->prev = pos->prev;
    pos->prev->next = self;
    pos->prev = self;
}
static void list_unhook(lnode *self)
{
    lnode *n = self->next, *p = self->prev;
    p->next = n;
    n->prev = p;
}
static void list_transfer(lnode *self, lnode *first, lnode *last)
{
    if (self != last) {
        last->prev->next = self;
        first->prev->next = last;
        self->prev->next = first;
        lnode *tmp = self->prev;
        self->prev = last->prev;
        last->prev = first->prev;
        first->prev = tmp;
    }
}
static void list_swap(lnode *x, lnode *y)
{
    if (x->next != x) {
        if (y->next != y) {
            lnode *xn = x->next, *xp = x->prev;
            x->next = y->next; x->prev = y->prev;
            y->next = xn;      y->prev = xp;
            x->next->prev = x->prev->next = x;
            y->next->prev = y->prev->next = y;
        } else {
            y->next = x->next; y->prev = x->prev;
            y->next->prev = y->prev->next = y;
            x->next = x->prev = x;
        }
    } else if (y->next != y) {
        x->next = y->next; x->prev = y->prev;
        x->next->prev = x->prev->next = x;
        y->next = y->prev = y;
    }
}

/* ---- the table ------------------------------------------------------------------------ */

const host_symbol gmv_host_symbols_cxx[] = {
    /* C++ ABI support */
    FN("___cxa_atexit", shim_cxa_atexit),
    FN("___cxa_guard_acquire", shim_cxa_guard_acquire),
    FN("___cxa_guard_release", shim_cxa_guard_release),
    FN("___cxa_guard_abort", shim_cxa_guard_abort),
    FN("___cxa_pure_virtual", shim_pure_virtual),

    /* iostream static-init object (no-op; see note above) */
    FN("__ZNSt8ios_base4InitC1Ev", shim_ios_base_init),

    /* std::allocator<char> ctor / copy-ctor / dtor (stateless, no-op) */
    FN("__ZNSaIcEC1Ev", shim_alloc_noop),
    FN("__ZNSaIcEC2Ev", shim_alloc_noop),
    FN("__ZNSaIcED1Ev", shim_alloc_noop),
    FN("__ZNSaIcED2Ev", shim_alloc_noop),
    FN("__ZNSaIcEC1ERKS_", shim_alloc_noop),
    FN("__ZNSaIcEC2ERKS_", shim_alloc_noop),

    /* std::__throw_* reporters */
    FN("__ZSt17__throw_bad_allocv", shim_throw_bad_alloc),
    FN("__ZSt20__throw_length_errorPKc", shim_throw_length_error),
    FN("__ZSt19__throw_logic_errorPKc", shim_throw_logic_error),
    FN("__ZSt20__throw_out_of_rangePKc", shim_throw_out_of_range),

    /* nothrow operator new / new[] / delete / delete[] (malloc/free; see note above) */
    FN("__ZnwmRKSt9nothrow_t", shim_new_nothrow),
    FN("__ZnamRKSt9nothrow_t", shim_new_nothrow),
    FN("__ZdlPvRKSt9nothrow_t", shim_delete_nothrow),
    FN("__ZdaPvRKSt9nothrow_t", shim_delete_nothrow),

    /* red-black tree node helpers (std::map / std::set) */
    FN("__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base", rb_increment),
    FN("__ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base", rb_increment),
    FN("__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base", rb_decrement),
    FN("__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_", rb_insert_and_rebalance),
    FN("__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_", rb_rebalance_for_erase),

    /* std::list node base */
    FN("__ZNSt15_List_node_base4hookEPS_", list_hook),
    FN("__ZNSt15_List_node_base6unhookEv", list_unhook),
    FN("__ZNSt15_List_node_base8transferEPS_S0_", list_transfer),
    FN("__ZNSt15_List_node_base4swapERS_S0_", list_swap),

    {NULL, NULL},
};
