/* imp_cxx.c -- libstdc++ (GCC 4.2, the one iOS 4 shipped) and the C++ ABI support the game
   imports: std::string, the red-black tree and list helpers, operator new/delete, guards.

   std::string is the copy-on-write string: one pointer _M_p to the characters, with a header in
   front of them,  struct _Rep { size_t length, capacity; int refcount; }  (12 bytes).  refcount 0 =
   one owner, > 0 shared, -1 "leaked" (someone holds a pointer into it: copies must clone).  The
   empty string is one static _Rep that is never freed.  The game inlines much of basic_string
   (size(), c_str(), _M_dispose's test...), so the layout and conventions are libstdc++'s exactly.
   A 32-bit host: guest pointers are host pointers, so this file uses plain C pointers.

   Functions returning a std::string by value get the result's address first (sret in r0). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "abi.h"
#include "rt.h"

typedef uint32_t size_type;
#define NPOS ((size_type)-1)
#define MAX_SIZE ((size_type)0x3FFFFFFC)

typedef struct { size_type length, capacity; volatile LONG refcount; } Rep;

static struct { Rep rep; char nul[4]; } empty_rep;

#define REP(d) ((Rep *)((char *)(d) - sizeof(Rep)))
#define DATA(r) ((char *)(r) + sizeof(Rep))

typedef struct { char *p; } string;

static Rep *rep_of(const string *s) { return REP(s->p); }
static size_type len_of(const string *s) { return rep_of(s)->length; }

/* std::length_error / std::out_of_range: the game never catches these; stop with the reason. */
__attribute__((noreturn)) static void gt_cxx_throw_length(const char *what) { rt_fatal("std::length_error: %s", what); }
__attribute__((noreturn)) static void gt_cxx_throw_range(const char *what) { rt_fatal("std::out_of_range: %s", what); }

/* _Rep::_S_create, with libstdc++'s growth policy */
static Rep *rep_create(size_type cap, size_type old_cap)
{
    if (cap > MAX_SIZE)
        gt_cxx_throw_length("basic_string::_S_create");
    if (cap > old_cap && cap < 2 * old_cap)
        cap = 2 * old_cap;
    size_type size = cap + 1 + sizeof(Rep);
    size_type adj = size + 16;
    if (adj > 4096 && cap > old_cap) {
        cap += 4096 - adj % 4096;
        if (cap > MAX_SIZE)
            cap = MAX_SIZE;
        size = cap + 1 + sizeof(Rep);
    }
    Rep *r = malloc(size);
    if (!r)
        rt_fatal("out of memory (std::string of %u)", cap);
    r->capacity = cap;
    r->refcount = 0;
    r->length = 0;
    return r;
}

static void rep_set_length(Rep *r, size_type n)
{
    if (r != &empty_rep.rep) {
        r->refcount = 0;
        r->length = n;
        DATA(r)[n] = 0;
    }
}

static void rep_dispose(Rep *r)
{
    if (r != &empty_rep.rep && InterlockedExchangeAdd(&r->refcount, -1) <= 0)
        free(r);
}

static char *rep_clone(Rep *r, size_type extra)
{
    Rep *n = rep_create(r->length + extra, r->capacity);
    if (r->length)
        memcpy(DATA(n), DATA(r), r->length);
    rep_set_length(n, r->length);
    return DATA(n);
}

static char *rep_grab(Rep *r)
{
    if (r->refcount >= 0) {
        if (r != &empty_rep.rep)
            InterlockedIncrement(&r->refcount);
        return DATA(r);
    }
    return rep_clone(r, 0);
}

static char *construct(const char *s, size_type n)
{
    if (!n)
        return DATA(&empty_rep.rep);
    Rep *r = rep_create(n, 0);
    memcpy(DATA(r), s, n);
    rep_set_length(r, n);
    return DATA(r);
}

/* _M_mutate(pos, len1, len2): make room for len2 chars in place of len1 at pos, unshared. */
static void mutate(string *s, size_type pos, size_type len1, size_type len2)
{
    Rep *r = rep_of(s);
    size_type old = r->length, nsize = old + len2 - len1, how_much = old - pos - len1;
    if (nsize > r->capacity || r->refcount > 0) {
        Rep *n = rep_create(nsize, r->capacity);
        if (pos)
            memcpy(DATA(n), s->p, pos);
        if (how_much)
            memcpy(DATA(n) + pos + len2, s->p + pos + len1, how_much);
        rep_dispose(r);
        s->p = DATA(n);
    } else if (how_much && len1 != len2) {
        memmove(s->p + pos + len2, s->p + pos + len1, how_much);
    }
    rep_set_length(rep_of(s), nsize);
}

static void leak(string *s)
{
    Rep *r = rep_of(s);
    if (r->refcount < 0 || r == &empty_rep.rep)
        return;
    if (r->refcount > 0)
        mutate(s, 0, 0, 0);
    rep_of(s)->refcount = -1;
}

static void check_pos(const string *s, size_type pos, const char *what)
{
    if (pos > len_of(s))
        gt_cxx_throw_range(what);
}

static size_type limit(const string *s, size_type pos, size_type n)
{
    size_type left = len_of(s) - pos;
    return n < left ? n : left;
}

/* replace [pos, pos+n1) by s[0..n2) (s may point into the string itself) */
static string *replace_raw(string *self, size_type pos, size_type n1, const char *src, size_type n2)
{
    if (len_of(self) - n1 > MAX_SIZE - n2)
        gt_cxx_throw_length("basic_string::replace");
    char *tmp = NULL;
    if (n2 && src >= self->p && src <= self->p + len_of(self)) {
        tmp = malloc(n2);
        memcpy(tmp, src, n2);
        src = tmp;
    }
    mutate(self, pos, n1, n2);
    if (n2)
        memcpy(self->p + pos, src, n2);
    free(tmp);
    return self;
}

/* constructors, destructor, assignment */

string *h_ZNSsC1Ev(string *self) { self->p = DATA(&empty_rep.rep); return self; }
string *h_ZNSsC1EPKcRKSaIcE(string *self, const char *s, void *a)
{
    (void)a;
    self->p = construct(s, s ? (size_type)strlen(s) : 0);
    return self;
}
string *h_ZNSsC1EPKcmRKSaIcE(string *self, const char *s, size_type n, void *a)
{
    (void)a;
    self->p = construct(s, n);
    return self;
}
string *h_ZNSsC1ERKSs(string *self, const string *o) { self->p = rep_grab(rep_of(o)); return self; }
void h_ZNSsD1Ev(string *self) { rep_dispose(rep_of(self)); }

string *h_ZNSsaSERKSs(string *self, const string *o)
{
    if (rep_of(self) != rep_of(o)) {
        char *d = rep_grab(rep_of(o));
        rep_dispose(rep_of(self));
        self->p = d;
    }
    return self;
}
string *h_ZNSs6assignEPKcm(string *self, const char *s, size_type n)
{
    return replace_raw(self, 0, len_of(self), s, n);
}
string *h_ZNSs6assignEPKc(string *self, const char *s) { return h_ZNSs6assignEPKcm(self, s, (size_type)strlen(s)); }
string *h_ZNSsaSEPKc(string *self, const char *s) { return h_ZNSs6assignEPKc(self, s); }
string *h_ZNSs6assignERKSsmm(string *self, const string *o, size_type pos, size_type n)
{
    check_pos(o, pos, "basic_string::assign");
    return h_ZNSs6assignEPKcm(self, o->p + pos, limit(o, pos, n));
}

/* access */

const char *h_ZNKSs5c_strEv(const string *self) { return self->p; }
size_type h_ZNKSs4sizeEv(const string *self) { return len_of(self); }
u32 h_ZNKSs5emptyEv(const string *self) { return len_of(self) == 0; }
const char *h_ZNKSsixEm(const string *self, size_type i) { return self->p + i; }
char *h_ZNSsixEm(string *self, size_type i) { leak(self); return self->p + i; }
char *h_ZNSs2atEm(string *self, size_type i)
{
    if (i >= len_of(self))
        gt_cxx_throw_range("basic_string::at");
    leak(self);
    return self->p + i;
}
const char *h_ZNKSs5beginEv(const string *self) { return self->p; }
const char *h_ZNKSs3endEv(const string *self) { return self->p + len_of(self); }
char *h_ZNSs5beginEv(string *self) { leak(self); return self->p; }
char *h_ZNSs3endEv(string *self) { leak(self); return self->p + len_of(self); }
char *h_ZNKSs9_M_ibeginEv(const string *self) { return self->p; }
char *h_ZNKSs7_M_iendEv(const string *self) { return self->p + len_of(self); }

/* internals the inlined code calls */

Rep *h_ZNSs12_S_empty_repEv(void) { return &empty_rep.rep; }
string *h_ZNSs12_Alloc_hiderC1EPcRKSaIcE(string *self, char *p, void *a) { (void)a; self->p = p; return self; }
void h_ZNSs13_S_copy_charsEPcPKcS1_(char *p, const char *k1, const char *k2) { memcpy(p, k1, (size_t)(k2 - k1)); }
void h_ZNSs13_S_copy_charsEPcN9__gnu_cxx17__normal_iteratorIPKcSsEES4_(char *p, const char *k1, const char *k2)
{
    memcpy(p, k1, (size_t)(k2 - k1));
}
void h_ZNSs4_Rep10_M_destroyERKSaIcE(Rep *r, void *a) { (void)a; if (r != &empty_rep.rep) free(r); }
char *h_ZNSs4_Rep10_M_refdataEv(Rep *r) { return DATA(r); }
void h_ZNSs4_Rep26_M_set_length_and_sharableEm(Rep *r, size_type n) { rep_set_length(r, n); }
Rep *h_ZNSs4_Rep9_S_createEmmRKSaIcE(size_type cap, size_type old, void *a) { (void)a; return rep_create(cap, old); }

/* modifiers */

void h_ZNSs5clearEv(string *self) { mutate(self, 0, len_of(self), 0); }
void h_ZNSs7reserveEm(string *self, size_type n)
{
    Rep *r = rep_of(self);
    if (n != r->capacity || r->refcount > 0) {
        if (n < r->length)
            n = r->length;
        Rep *nr = rep_create(n, r->capacity);
        if (r->length)
            memcpy(DATA(nr), self->p, r->length);
        rep_set_length(nr, r->length);
        rep_dispose(r);
        self->p = DATA(nr);
    }
}
string *h_ZNSs6appendEPKcm(string *self, const char *s, size_type n)
{
    if (!n)
        return self;
    return replace_raw(self, len_of(self), 0, s, n);
}
string *h_ZNSs6appendEPKc(string *self, const char *s) { return h_ZNSs6appendEPKcm(self, s, (size_type)strlen(s)); }
string *h_ZNSspLEPKc(string *self, const char *s) { return h_ZNSs6appendEPKc(self, s); }
string *h_ZNSs6appendERKSs(string *self, const string *o) { return h_ZNSs6appendEPKcm(self, o->p, len_of(o)); }
string *h_ZNSspLERKSs(string *self, const string *o) { return h_ZNSs6appendERKSs(self, o); }
string *h_ZNSs6appendERKSsmm(string *self, const string *o, size_type pos, size_type n)
{
    check_pos(o, pos, "basic_string::append");
    return h_ZNSs6appendEPKcm(self, o->p + pos, limit(o, pos, n));
}
string *h_ZNSs6appendEmc(string *self, size_type n, u32 ch)
{
    if (!n)
        return self;
    size_type at = len_of(self);
    mutate(self, at, 0, n);
    memset(self->p + at, (int)(ch & 0xFF), n);
    return self;
}
string *h_ZNSspLEc(string *self, u32 ch) { return h_ZNSs6appendEmc(self, 1, ch); }
void h_ZNSs9push_backEc(string *self, u32 ch) { h_ZNSs6appendEmc(self, 1, ch); }
string *h_ZNSs6insertEmPKc(string *self, size_type pos, const char *s)
{
    check_pos(self, pos, "basic_string::insert");
    return replace_raw(self, pos, 0, s, (size_type)strlen(s));
}
void h_ZNSs6resizeEm(string *self, size_type n)
{
    size_type len = len_of(self);
    if (n > len)
        h_ZNSs6appendEmc(self, n - len, 0);
    else if (n < len)
        mutate(self, n, len - n, 0);
}
string *h_ZNSs5eraseEmm(string *self, size_type pos, size_type n)
{
    check_pos(self, pos, "basic_string::erase");
    mutate(self, pos, limit(self, pos, n), 0);
    return self;
}
char *h_ZNSs5eraseEN9__gnu_cxx17__normal_iteratorIPcSsEES2_(string *self, char *first, char *last)
{
    size_type pos = (size_type)(first - self->p);
    mutate(self, pos, (size_type)(last - first), 0);
    rep_of(self)->refcount = -1;           /* returns an iterator: leaked, as libstdc++ does */
    return self->p + pos;
}
string *h_ZNSs7replaceEmmPKc(string *self, size_type pos, size_type n, const char *s)
{
    check_pos(self, pos, "basic_string::replace");
    return replace_raw(self, pos, limit(self, pos, n), s, (size_type)strlen(s));
}
string *h_ZNSs7replaceEmmRKSs(string *self, size_type pos, size_type n, const string *o)
{
    check_pos(self, pos, "basic_string::replace");
    return replace_raw(self, pos, limit(self, pos, n), o->p, len_of(o));
}
string *h_ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_PKcS4_(string *self, char *i1, char *i2,
                                                                       const char *k1, const char *k2)
{
    return replace_raw(self, (size_type)(i1 - self->p), (size_type)(i2 - i1), k1, (size_type)(k2 - k1));
}

/* search and compare */

static size_type find_n(const string *self, const char *s, size_type pos, size_type n)
{
    size_type len = len_of(self);
    if (n == 0)
        return pos <= len ? pos : NPOS;
    for (; pos + n <= len; pos++)
        if (self->p[pos] == s[0] && !memcmp(self->p + pos, s, n))
            return pos;
    return NPOS;
}
size_type h_ZNKSs4findEPKcm(const string *self, const char *s, size_type pos)
{
    return find_n(self, s, pos, (size_type)strlen(s));
}
size_type h_ZNKSs4findERKSsm(const string *self, const string *o, size_type pos)
{
    return find_n(self, o->p, pos, len_of(o));
}
size_type h_ZNKSs4findEcm(const string *self, u32 ch, size_type pos)
{
    size_type len = len_of(self);
    for (; pos < len; pos++)
        if ((u8)self->p[pos] == (u8)ch)
            return pos;
    return NPOS;
}
size_type h_ZNKSs5rfindEPKcm(const string *self, const char *s, size_type pos)
{
    size_type len = len_of(self), n = (size_type)strlen(s);
    if (n > len)
        return NPOS;
    size_type i = len - n < pos ? len - n : pos;
    for (;; i--) {
        if (!memcmp(self->p + i, s, n))
            return i;
        if (!i)
            break;
    }
    return NPOS;
}
size_type h_ZNKSs13find_first_ofEPKcm(const string *self, const char *s, size_type pos)
{
    size_type len = len_of(self), n = (size_type)strlen(s);
    for (; n && pos < len; pos++)
        if (memchr(s, self->p[pos], n))
            return pos;
    return NPOS;
}
static int compare_n(const char *a, size_type na, const char *b, size_type nb)
{
    int r = memcmp(a, b, na < nb ? na : nb);
    return r ? r : (int)(na - nb);
}
u32 h_ZNKSs7compareERKSs(const string *self, const string *o)
{
    return (u32)compare_n(self->p, len_of(self), o->p, len_of(o));
}
u32 h_ZNKSs7compareEPKc(const string *self, const char *s)
{
    return (u32)compare_n(self->p, len_of(self), s, (size_type)strlen(s));
}
void h_ZNKSs6substrEmm(string *out, const string *self, size_type pos, size_type n)
{
    check_pos(self, pos, "basic_string::substr");
    out->p = construct(self->p + pos, limit(self, pos, n));
}

/* For other host code: a new std::string value in *out. */
void gt_string_make(u32 out, const char *s, u32 n) { ((string *)GP(out))->p = construct(s, n); }
const char *gt_string_cstr(u32 str) { return ((string *)GP(str))->p; }

/* std::allocator<char>: stateless */

void h_alloc_noop(void *self) { (void)self; }

/* operator new / delete */

void *h_Znwm(size_type n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        rt_fatal("operator new(%u): out of memory", n);
    return p;
}
void h_ZdlPv(void *p) { free(p); }

/* C++ ABI support */

/* The game never exits normally (an iOS app is killed): destructors at exit are not run. */
u32 h_cxa_atexit(u32 fn, u32 arg, u32 dso) { (void)fn; (void)arg; (void)dso; return 0; }

/* Guards of function-local statics: byte 0 = done (the inlined fast path tests it), byte 1 = busy.
   A second thread waits for the first, as on iOS. */
static SRWLOCK guard_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE guard_cv = CONDITION_VARIABLE_INIT;
u32 h_cxa_guard_acquire(volatile u8 *g)
{
    if (g[0])
        return 0;
    AcquireSRWLockExclusive(&guard_lock);
    while (g[1])
        SleepConditionVariableSRW(&guard_cv, &guard_lock, INFINITE, 0);
    int todo = !g[0];
    if (todo)
        g[1] = 1;
    ReleaseSRWLockExclusive(&guard_lock);
    return (u32)todo;
}
void h_cxa_guard_release(volatile u8 *g)
{
    AcquireSRWLockExclusive(&guard_lock);
    g[0] = 1;
    g[1] = 0;
    ReleaseSRWLockExclusive(&guard_lock);
    WakeAllConditionVariable(&guard_cv);
}
void h_cxa_guard_abort(volatile u8 *g)
{
    AcquireSRWLockExclusive(&guard_lock);
    g[1] = 0;
    ReleaseSRWLockExclusive(&guard_lock);
    WakeAllConditionVariable(&guard_cv);
}
void h_cxa_pure_virtual(CPU *c) { rt_fatal("pure virtual function called (return address 0x%08x)", c->r[14]); }
void h_ZSt9terminatev(CPU *c) { rt_fatal("std::terminate (return address 0x%08x)", c->r[14]); }

/* Each translation unit with <iostream> has a static ios_base::Init: nothing to set up here (the
   standard streams the game uses are made on demand). */
void h_ZNSt8ios_base4InitC1Ev(void *self) { (void)self; }

/* red-black tree (libstdc++ 4.2 tree.cc; node = color, parent, left, right) */

enum { RB_RED, RB_BLACK };
typedef struct rbnode { u32 color; struct rbnode *parent, *left, *right; } rbnode;

rbnode *h_ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base(rbnode *x)
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

rbnode *h_ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base(rbnode *x)
{
    if (x->color == RB_RED && x->parent->parent == x) {
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

static void rot_left(rbnode *x, rbnode **root)
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

static void rot_right(rbnode *x, rbnode **root)
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

void h_ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_(u32 insert_left, rbnode *x, rbnode *p,
                                                                         rbnode *header)
{
    rbnode **root = &header->parent;
    x->parent = p;
    x->left = x->right = NULL;
    x->color = RB_RED;
    if (insert_left & 0xFF) {
        p->left = x;
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
                    rot_left(x, root);
                }
                x->parent->color = RB_BLACK;
                xpp->color = RB_RED;
                rot_right(xpp, root);
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
                    rot_right(x, root);
                }
                x->parent->color = RB_BLACK;
                xpp->color = RB_RED;
                rot_left(xpp, root);
            }
        }
    }
    (*root)->color = RB_BLACK;
}

rbnode *h_ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_(rbnode *z, rbnode *header)
{
    rbnode **root = &header->parent, **leftmost = &header->left, **rightmost = &header->right;
    rbnode *y = z, *x = NULL, *xp = NULL;
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
            xp = y->parent;
            if (x)
                x->parent = y->parent;
            y->parent->left = x;
            y->right = z->right;
            z->right->parent = y;
        } else {
            xp = y;
        }
        if (*root == z)
            *root = y;
        else if (z->parent->left == z)
            z->parent->left = y;
        else
            z->parent->right = y;
        y->parent = z->parent;
        u32 t = y->color;
        y->color = z->color;
        z->color = t;
        y = z;
    } else {
        xp = y->parent;
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
                while (m->left)
                    m = m->left;
                *leftmost = m;
            }
        }
        if (*rightmost == z) {
            if (!z->left) {
                *rightmost = z->parent;
            } else {
                rbnode *m = x;
                while (m->right)
                    m = m->right;
                *rightmost = m;
            }
        }
    }
    if (y->color != RB_RED) {
        while (x != *root && (!x || x->color == RB_BLACK)) {
            if (x == xp->left) {
                rbnode *w = xp->right;
                if (w->color == RB_RED) {
                    w->color = RB_BLACK;
                    xp->color = RB_RED;
                    rot_left(xp, root);
                    w = xp->right;
                }
                if ((!w->left || w->left->color == RB_BLACK) && (!w->right || w->right->color == RB_BLACK)) {
                    w->color = RB_RED;
                    x = xp;
                    xp = xp->parent;
                } else {
                    if (!w->right || w->right->color == RB_BLACK) {
                        if (w->left)
                            w->left->color = RB_BLACK;
                        w->color = RB_RED;
                        rot_right(w, root);
                        w = xp->right;
                    }
                    w->color = xp->color;
                    xp->color = RB_BLACK;
                    if (w->right)
                        w->right->color = RB_BLACK;
                    rot_left(xp, root);
                    break;
                }
            } else {
                rbnode *w = xp->left;
                if (w->color == RB_RED) {
                    w->color = RB_BLACK;
                    xp->color = RB_RED;
                    rot_right(xp, root);
                    w = xp->left;
                }
                if ((!w->right || w->right->color == RB_BLACK) && (!w->left || w->left->color == RB_BLACK)) {
                    w->color = RB_RED;
                    x = xp;
                    xp = xp->parent;
                } else {
                    if (!w->left || w->left->color == RB_BLACK) {
                        if (w->right)
                            w->right->color = RB_BLACK;
                        w->color = RB_RED;
                        rot_left(w, root);
                        w = xp->left;
                    }
                    w->color = xp->color;
                    xp->color = RB_BLACK;
                    if (w->left)
                        w->left->color = RB_BLACK;
                    rot_right(xp, root);
                    break;
                }
            }
        }
        if (x)
            x->color = RB_BLACK;
    }
    return y;
}

/* std::list node base (libstdc++ 4.2 list.cc; node = next, prev) */

typedef struct lnode { struct lnode *next, *prev; } lnode;

void h_ZNSt15_List_node_base4hookEPS_(lnode *self, lnode *pos)
{
    self->next = pos;
    self->prev = pos->prev;
    pos->prev->next = self;
    pos->prev = self;
}
void h_ZNSt15_List_node_base6unhookEv(lnode *self)
{
    lnode *n = self->next, *p = self->prev;
    p->next = n;
    n->prev = p;
}
void h_ZNSt15_List_node_base8transferEPS_S0_(lnode *self, lnode *first, lnode *last)
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
