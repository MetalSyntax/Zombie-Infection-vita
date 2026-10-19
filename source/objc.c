/**
 * @file objc.c
 * @brief Minimal Objective-C 2.0 runtime and Foundation for the iOS 3 guest.
 * @details
 *  - Dispatch: objc_msgSend/_stret/Super2 (asm.S) call objc_lookup*, which walks the class chain.
 *    Guest classes are searched through their compiled class_ro_t method lists; host classes
 *    through the host_method tables registered with objc_register(). Results are cached per
 *    (class, selector) since nothing is added at runtime (the binary has no categories).
 *  - Host classes are created on demand for every _OBJC_CLASS_$_X / _OBJC_METACLASS_$_X the guest
 *    imports; unknown selectors log once and return nil/0.
 *  - NSString keeps the __cfstring layout {isa, flags, cstr, len}: @"..." constants from the binary
 *    and runtime strings are interchangeable (cstr at +8, as %@ in darwin_printf expects).
 *  - Memory: retain/release/autorelease are no-ops (objects are never freed).
 *    ponytail: no reference counting; long sessions leak every NSString/NSData the game creates.
 *    Upgrade to a retain count + autorelease pool drain if memory grows on long runs.
 *  - Typography: NSString + UIFont (sizeWithFont:, drawAtPoint:withFont:) rendered via FreeType.
 */
#include <ft2build.h>
#include FT_FREETYPE_H
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

#include "darwin.h"
#include "darwin_printf.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"
#include "objc.h"
#include "objc_rt.h"

// ---------------------------------------------------------------- host classes

typedef struct {
    objc_class cls, meta;
    objc_class_ro ro, mro;
    char name[48];
    uint32_t size;
} host_class;

#define MAX_HOST_CLASSES 160
static host_class hosts[MAX_HOST_CLASSES];
static int nhosts;

// Superclass and instance size (bytes, isa included) of the host classes the runtime knows.
// UIView/UIViewController sizes match the instanceStart of the game's subclasses, so host
// state stored inside those ranges never overlaps guest ivars.
static const struct {
    const char *name, *super;
    uint32_t size;
} host_defs[] = {
    {"NSObject", NULL, 4},
    {"NSString", "NSObject", 16},
    {"NSMutableString", "NSString", 16},
    {"NSNumber", "NSObject", 32},
    {"NSArray", "NSObject", 16},
    {"NSMutableArray", "NSArray", 16},
    {"NSSet", "NSArray", 16},
    {"NSMutableSet", "NSSet", 16},
    {"NSDictionary", "NSObject", 20},
    {"NSMutableDictionary", "NSDictionary", 20},
    {"NSData", "NSObject", 16},
    {"NSMutableData", "NSData", 16},
    {"NSDate", "NSObject", 16},
    {"NSFileHandle", "NSObject", 8},
    {"NSDirectoryEnumerator", "NSObject", 12},
    {"NSNotification", "NSObject", 16},
    {"UIResponder", "NSObject", 4},
    {"UIView", "UIResponder", 36},
    {"UIWindow", "UIView", 36},
    {"UILabel", "UIView", 36},
    {"UIButton", "UIView", 36},
    {"UIImageView", "UIView", 36},
    {"UITextField", "UIView", 36},
    {"UITextView", "UIView", 36},
    {"UIWebView", "UIView", 36},
    {"UIActivityIndicatorView", "UIView", 36},
    {"UIAlertView", "UIView", 36},
    {"UIViewController", "UIResponder", 92},
    {"UINavigationController", "UIViewController", 92},
    {"UIImagePickerController", "UINavigationController", 92},
};

static int is_host(const objc_class *c) {
    return (const void *)c >= (const void *)hosts && (const void *)c < (const void *)&hosts[nhosts];
}
static host_class *host_of(const objc_class *c) {
    return &hosts[((const uint8_t *)c - (const uint8_t *)hosts) / sizeof(host_class)];
}
static int is_meta(const objc_class *c) {
    return is_host(c) ? c == &host_of(c)->meta : (c->data->flags & 1);
}

static host_class *host_find(const char *name) {
    for (int i = 0; i < nhosts; i++)
        if (!strcmp(hosts[i].name, name))
            return &hosts[i];
    return NULL;
}

static host_class *host_get(const char *name) {
    host_class *h = host_find(name);
    if (h)
        return h;
    if (nhosts == MAX_HOST_CLASSES)
        fatal_error("objc: too many host classes (%s)", name);
    const char *super = "NSObject";
    uint32_t size = 64; // room for the host state of any other class (NSTimer, UITouch, ...)
    for (unsigned i = 0; i < sizeof(host_defs) / sizeof(host_defs[0]); i++)
        if (!strcmp(host_defs[i].name, name))
            super = host_defs[i].super, size = host_defs[i].size;
    host_class *sh = super ? host_get(super) : NULL; // superclasses are created first
    h = &hosts[nhosts++];
    snprintf(h->name, sizeof(h->name), "%s", name);
    h->size = size;
    h->ro = (objc_class_ro){.instanceStart = size, .instanceSize = size, .name = h->name};
    h->mro = (objc_class_ro){.flags = 1, .instanceStart = 20, .instanceSize = 20, .name = h->name};
    host_class *root = sh ? host_get("NSObject") : h;
    h->cls = (objc_class){&h->meta, sh ? &sh->cls : NULL, NULL, NULL, &h->ro};
    // Root metaclass inherits from the root class: class objects answer NSObject instance methods.
    h->meta = (objc_class){&root->meta, sh ? &sh->meta : &h->cls, NULL, NULL, &h->mro};
    return h;
}

objc_class *objc_host_class(const char *name) { return &host_get(name)->cls; }

objc_class *objc_get_class(const char *name) {
    char sym[96];
    snprintf(sym, sizeof(sym), "_OBJC_CLASS_$_%s", name);
    uintptr_t g = macho_sym(sym);
    return g ? (objc_class *)g : objc_host_class(name);
}

// ---------------------------------------------------------------- host method tables

static const host_method *tables[16];
static int table_len[16], ntables;

void objc_register(const host_method *m, int n) {
    if (ntables == 16)
        fatal_error("objc: too many host method tables");
    tables[ntables] = m;
    table_len[ntables++] = n;
}

static void *host_method_find(const char *cls, char kind, SEL sel) {
    for (int t = 0; t < ntables; t++)
        for (int i = 0; i < table_len[t]; i++) {
            const host_method *m = &tables[t][i];
            if (m->kind == kind && !strcmp(m->cls, cls) && !strcmp(m->sel, sel))
                return m->fn;
        }
    return NULL;
}

// ---------------------------------------------------------------- lookup + cache

typedef struct {
    void *imp;
    int host;
} imp_ref;

static imp_ref find_method(objc_class *cls, SEL sel) {
    for (objc_class *c = cls; c; c = c->superclass) {
        if (is_host(c)) {
            void *fn = host_method_find(host_of(c)->name, is_meta(c) ? '+' : '-', sel);
            if (fn)
                return (imp_ref){fn, 1};
        } else if (c->data && c->data->baseMethods) {
            objc_method_list *ml = c->data->baseMethods;
            uint32_t es = ml->entsize & ~3;
            for (uint32_t i = 0; i < ml->count; i++) {
                objc_method *m = (objc_method *)((uint8_t *)ml->m + i * es);
                if (m->name == sel || !strcmp(m->name, sel))
                    return (imp_ref){m->imp, 0};
            }
        }
    }
    return (imp_ref){NULL, 0};
}

const char *objc_class_name(id obj) {
    if (!obj)
        return "nil";
    objc_class *c = is_meta(obj->isa) ? (objc_class *)obj : obj->isa;
    return is_host(c) ? host_of(c)->name : c->data->name;
}

HM(unknown_method) {
    static const char *seen_cls[512];
    static SEL seen_sel[512];
    static int nseen;
    const char *cn = objc_class_name(self);
    for (int i = 0; i < nseen; i++)
        if (seen_sel[i] == sel && seen_cls[i] == cn)
            return 0;
    if (nseen < 512)
        seen_cls[nseen] = cn, seen_sel[nseen++] = sel;
    l_warn("objc: %c[%s %s] not implemented -> nil", is_meta(self->isa) ? '+' : '-', cn, sel);
    return 0;
}

#define CACHE_SIZE 8192
static struct {
    objc_class *cls;
    SEL sel;
    imp_ref r;
} cache[CACHE_SIZE];
static volatile int cache_lock;

static imp_ref lookup(objc_class *cls, SEL sel) {
    uint32_t h = (((uint32_t)cls >> 2) ^ ((uint32_t)sel * 2654435761u)) & (CACHE_SIZE - 1);
    while (__sync_lock_test_and_set(&cache_lock, 1)) {}
    for (uint32_t i = h;; i = (i + 1) & (CACHE_SIZE - 1)) {
        if (cache[i].cls == cls && cache[i].sel == sel) {
            imp_ref r = cache[i].r;
            __sync_lock_release(&cache_lock);
            return r;
        }
        if (!cache[i].cls || i == ((h - 1) & (CACHE_SIZE - 1)))
            break;
    }
    __sync_lock_release(&cache_lock);
    imp_ref r = find_method(cls, sel);
    if (!r.imp)
        r = (imp_ref){unknown_method, 1};
    while (__sync_lock_test_and_set(&cache_lock, 1)) {}
    for (uint32_t i = h, n = 0; n < CACHE_SIZE / 2; i = (i + 1) & (CACHE_SIZE - 1), n++)
        if (!cache[i].cls) {
            cache[i].sel = sel, cache[i].r = r, cache[i].cls = cls;
            break;
        }
    __sync_lock_release(&cache_lock);
    return r;
}

static uint64_t pack(imp_ref r) { return (uint32_t)r.imp | (uint64_t)r.host << 32; }

// Called from asm.S with an aligned stack.
uint64_t objc_lookup(id self, SEL sel) { return pack(lookup(self->isa, sel)); }
uint64_t objc_lookup_stret(id self, SEL sel) { return pack(lookup(self->isa, sel)); }
uint64_t objc_lookup_super2(id *sup, SEL sel) {
    objc_class *c = ((objc_class **)sup)[1];
    return pack(lookup(c->superclass, sel));
}

int objc_responds(id obj, SEL sel) { return obj && find_method(obj->isa, sel).imp != NULL; }

int objc_is_kind(id obj, objc_class *cls) {
    for (objc_class *c = obj ? obj->isa : NULL; c; c = c->superclass)
        if (c == cls)
            return 1;
    return 0;
}

typedef uint64_t (*hm_fn)(id, SEL, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                          uint32_t);

uint32_t objc_send(id self, SEL sel, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (!self)
        return 0;
    imp_ref r = lookup(self->isa, sel);
    if (r.host)
        return ((hm_fn)r.imp)(self, sel, a, b, c, d, 0, 0, 0, 0);
    return call_guest6(r.imp, (uint32_t)self, (uint32_t)sel, a, b, c, d);
}

id objc_alloc(objc_class *cls) {
    uint32_t size = is_host(cls) ? host_of(cls)->size : cls->data->instanceSize;
    id o = calloc(1, size < 16 ? 16 : size);
    o->isa = cls;
    return o;
}

extern void objc_msgSend_entry(void);
extern void objc_msgSend_stret_entry(void);
extern void objc_msgSendSuper2_entry(void);

// ---------------------------------------------------------------- NSObject

HM(o_alloc) { return RET_ID(objc_alloc((objc_class *)self)); }
HM(o_new) { return objc_send(objc_alloc((objc_class *)self), "init", 0, 0, 0, 0); }
HM(o_self) { return RET_ID(self); }
HM(o_void) { return 0; }
HM(o_yes) { return 1; }
HM(o_class) { return RET_ID(is_meta(self->isa) ? (objc_class *)self : self->isa); }
HM(o_responds) { return objc_responds(self, (SEL)a); }
HM(o_instances_respond) { return find_method((objc_class *)self, (SEL)a).imp != NULL; }
HM(o_is_kind) { return objc_is_kind(self, (objc_class *)a); }
HM(o_is_member) { return self->isa == (objc_class *)a; }
HM(o_is_equal) { return (uint32_t)self == a; }
HM(o_hash) { return (uint32_t)self >> 4; }
HM(o_retain_count) { return 1; }
HM(o_description) { return RET_ID(ns_string(objc_class_name(self))); }
HM(o_perform0) { return objc_send(self, (SEL)a, 0, 0, 0, 0); }
HM(o_perform1) { return objc_send(self, (SEL)a, b, 0, 0, 0); }
HM(o_perform2) { return objc_send(self, (SEL)a, b, c, 0, 0); }
// performSelectorOnMainThread:withObject:waitUntilDone: runs synchronously (single run loop).
HM(o_perform_main) { return objc_send(self, (SEL)a, b, 0, 0, 0); }

typedef struct {
    id target;
    SEL sel;
    id arg;
} thread_start;
static void *ns_thread(void *p) {
    thread_start t = *(thread_start *)p;
    free(p);
    objc_send(t.target, t.sel, (uint32_t)t.arg, 0, 0, 0);
    return NULL;
}
// +[NSThread detachNewThreadSelector:toTarget:withObject:]
HM(thread_detach) {
    thread_start *t = malloc(sizeof(*t));
    *t = (thread_start){(id)b, (SEL)a, (id)c};
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_create(&th, &attr, ns_thread, t);
    pthread_attr_destroy(&attr);
    l_info("NSThread: -[%s %s] started", objc_class_name((id)b), (SEL)a);
    return 0;
}

// ---------------------------------------------------------------- NSString

typedef struct {
    objc_class *isa;
    uint32_t flags;
    char *str;
    uint32_t len;
} ns_string_t;

static objc_class *c_NSString, *c_NSArray, *c_NSMutableArray, *c_NSNumber, *c_NSDictionary, *c_NSData,
    *c_NSMutableData, *c_NSDate, *c_NSFileHandle, *c_NSDirEnum, *c_NSNotification;

static ns_string_t *str_init(ns_string_t *o, const char *s, int n) {
    if (!o->isa)
        o->isa = c_NSString;
    if (n < 0)
        n = s ? strlen(s) : 0;
    o->str = malloc(n + 1);
    memcpy(o->str, s ? s : "", n);
    o->str[n] = 0;
    o->len = n;
    o->flags = 0x7C8;
    return o;
}

id ns_string(const char *s) { return (id)str_init(calloc(1, sizeof(ns_string_t)), s, -1); }
static id ns_string_n(const char *s, int n) { return (id)str_init(calloc(1, sizeof(ns_string_t)), s, n); }
const char *ns_cstr(id o) { return o ? ((ns_string_t *)o)->str : ""; }
static uint32_t ns_len(id o) { return o ? ((ns_string_t *)o)->len : 0; }
static int is_string(id o) { return o && objc_is_kind(o, c_NSString); }

static ns_string_t *str_target(id self) {
    // + stringWith...: self is the class; - initWith...: self is the allocated instance.
    return is_meta(self->isa) ? calloc(1, sizeof(ns_string_t)) : (ns_string_t *)self;
}

static char *read_file(const char *path, long *len) {
    char p[512];
    FILE *f = fopen(darwin_path(path, p, sizeof(p)), "rb");
    if (!f) {
        l_debug("read_file(%s): not found", p);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(*len + 1);
    fread(b, 1, *len, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

static int write_file(const char *path, const void *data, size_t len) {
    char p[512];
    FILE *f = fopen(darwin_path(path, p, sizeof(p)), "wb");
    if (!f) {
        l_warn("write_file(%s) failed", p);
        return 0;
    }
    fwrite(data, 1, len, f);
    fclose(f);
    return 1;
}

static id str_format(id self, const char *fmt, va_list ap) {
    char out[2048];
    va_list cp;
    va_copy(cp, ap);
    words w = {&cp, NULL};
    darwin_vformat(out, sizeof(out), fmt, &w);
    va_end(cp);
    return (id)str_init(str_target(self), out, -1);
}

static uint64_t s_with_format(id self, SEL sel, uint32_t fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    id r = str_format(self, ns_cstr((id)fmt), ap);
    va_end(ap);
    return RET_ID(r);
}
static uint64_t s_append_format(id self, SEL sel, uint32_t fmt, ...) {
    char out[2048];
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    darwin_vformat(out, sizeof(out), ns_cstr((id)fmt), &w);
    va_end(ap);
    char *s = malloc(ns_len(self) + strlen(out) + 1);
    sprintf(s, "%s%s", ns_cstr(self), out);
    id r = ns_string(s);
    free(s);
    return RET_ID(r);
}

HM(s_with_cstr) { return RET_ID(str_init(str_target(self), (const char *)a, -1)); }
HM(s_with_string) { return RET_ID(str_init(str_target(self), ns_cstr((id)a), -1)); }
HM(s_with_bytes) { return RET_ID(str_init(str_target(self), (const char *)a, b)); }
HM(s_empty) { return RET_ID(str_init(str_target(self), "", 0)); }
HM(s_with_data) {
    uint32_t *d = (uint32_t *)a; // NSData {isa, len, cap, bytes}
    return RET_ID(d ? str_init(str_target(self), (const char *)d[3], d[1]) : NULL);
}
HM(s_with_file) {
    long n;
    char *buf = read_file(ns_cstr((id)a), &n);
    if (!buf)
        return 0;
    id r = (id)str_init(str_target(self), buf, n);
    free(buf);
    return RET_ID(r);
}
HM(s_utf8) { return RET_ID(ns_cstr(self)); }
HM(s_length) { return ns_len(self); }
HM(s_char_at) { return a < ns_len(self) ? (uint8_t)ns_cstr(self)[a] : 0; }
HM(s_get_cstring) {
    snprintf((char *)a, b, "%s", ns_cstr(self));
    return ns_len(self) < b;
}
HM(s_is_equal) { return is_string((id)a) && !strcmp(ns_cstr(self), ns_cstr((id)a)); }
HM(s_hash) {
    uint32_t h = 5381;
    for (const char *p = ns_cstr(self); *p; p++)
        h = h * 33 + (uint8_t)*p;
    return h;
}
HM(s_compare) {
    int r = strcmp(ns_cstr(self), ns_cstr((id)a));
    return r < 0 ? (uint32_t)-1 : r > 0;
}
HM(s_case_compare) {
    int r = strcasecmp(ns_cstr(self), ns_cstr((id)a));
    return r < 0 ? (uint32_t)-1 : r > 0;
}
HM(s_has_prefix) { return !strncmp(ns_cstr(self), ns_cstr((id)a), ns_len((id)a)); }
HM(s_has_suffix) {
    uint32_t n = ns_len(self), m = ns_len((id)a);
    return m <= n && !strcmp(ns_cstr(self) + n - m, ns_cstr((id)a));
}
HM(s_append) {
    char *s = malloc(ns_len(self) + ns_len((id)a) + 1);
    sprintf(s, "%s%s", ns_cstr(self), ns_cstr((id)a));
    id r = ns_string(s);
    free(s);
    return RET_ID(r);
}
HM(s_append_path) {
    char *s = malloc(ns_len(self) + ns_len((id)a) + 2);
    const char *base = ns_cstr(self);
    int slash = ns_len(self) && base[ns_len(self) - 1] == '/';
    sprintf(s, "%s%s%s", base, slash || !ns_len(self) ? "" : "/", ns_cstr((id)a));
    id r = ns_string(s);
    free(s);
    return RET_ID(r);
}
HM(s_last_path) {
    const char *s = ns_cstr(self), *p = strrchr(s, '/');
    return RET_ID(ns_string(p ? p + 1 : s));
}
HM(s_delete_last_path) {
    const char *s = ns_cstr(self), *p = strrchr(s, '/');
    return RET_ID(ns_string_n(s, p ? p - s : 0));
}
HM(s_path_ext) {
    const char *s = ns_cstr(self), *p = strrchr(s, '.');
    return RET_ID(ns_string(p && !strchr(p, '/') ? p + 1 : ""));
}
HM(s_delete_ext) {
    const char *s = ns_cstr(self), *p = strrchr(s, '.');
    return RET_ID(ns_string_n(s, p && !strchr(p, '/') ? p - s : (int)ns_len(self)));
}
HM(s_is_absolute) { return ns_cstr(self)[0] == '/' || strstr(ns_cstr(self), ":") != NULL; }
HM(s_replace) {
    const char *s = ns_cstr(self), *from = ns_cstr((id)a), *to = ns_cstr((id)b);
    size_t fl = strlen(from), tl = strlen(to), cap = ns_len(self) + 1, n = 0;
    char *out = malloc(cap);
    while (*s) {
        if (fl && !strncmp(s, from, fl)) {
            cap += tl;
            out = realloc(out, cap);
            memcpy(out + n, to, tl);
            n += tl, s += fl;
        } else {
            out[n++] = *s++;
        }
    }
    out[n] = 0;
    id r = ns_string(out);
    free(out);
    return RET_ID(r);
}
HM(s_substr_to) { return RET_ID(ns_string_n(ns_cstr(self), a < ns_len(self) ? a : ns_len(self))); }
HM(s_substr_from) { return RET_ID(ns_string(a < ns_len(self) ? ns_cstr(self) + a : "")); }
HM(s_substr_range) { return RET_ID(a < ns_len(self) ? ns_string_n(ns_cstr(self) + a, b) : ns_string("")); }
HM_STRET(s_range_of) {
    const char *s = ns_cstr(self), *p = strstr(s, ns_cstr((id)a));
    ((uint32_t *)ret)[0] = p ? (uint32_t)(p - s) : 0x7FFFFFFF; // NSNotFound
    ((uint32_t *)ret)[1] = p ? ns_len((id)a) : 0;
}
HM(s_components) {
    id arr = ns_array_new();
    const char *s = ns_cstr(self), *sep = ns_cstr((id)a);
    size_t sl = strlen(sep);
    for (const char *p; sl && (p = strstr(s, sep)); s = p + sl)
        ns_array_add(arr, ns_string_n(s, p - s));
    ns_array_add(arr, ns_string(s));
    return RET_ID(arr);
}
HM(s_int_value) { return atoi(ns_cstr(self)); }
HM(s_float_value) { return RET_F(strtof(ns_cstr(self), NULL)); }
HM(s_double_value) { return RET_D(strtod(ns_cstr(self), NULL)); }
HM(s_bool_value) {
    char ch = ns_cstr(self)[0];
    return ch == 'Y' || ch == 'y' || ch == 'T' || ch == 't' || (ch >= '1' && ch <= '9');
}
HM(s_lower) {
    id r = ns_string(ns_cstr(self));
    for (char *p = ((ns_string_t *)r)->str; *p; p++)
        *p = tolower((unsigned char)*p);
    return RET_ID(r);
}
HM(s_upper) {
    id r = ns_string(ns_cstr(self));
    for (char *p = ((ns_string_t *)r)->str; *p; p++)
        *p = toupper((unsigned char)*p);
    return RET_ID(r);
}
HM(s_copy) { return RET_ID(ns_string(ns_cstr(self))); }
HM(s_to_data);
HM(s_write) { return write_file(ns_cstr((id)a), ns_cstr(self), ns_len(self)); }

// ---------------------------------------------------------------- NSNumber

typedef struct {
    objc_class *isa;
    uint32_t flags;
    char *desc; // at +8 so a stray %@ prints it
    uint32_t is_double;
    int64_t i;
    double d;
} ns_number_t;

static id number_new(int64_t i, double d, int is_double) {
    ns_number_t *n = calloc(1, sizeof(*n));
    n->isa = c_NSNumber;
    n->i = is_double ? (int64_t)d : i;
    n->d = is_double ? d : (double)i;
    n->is_double = is_double;
    char buf[32];
    is_double ? snprintf(buf, sizeof(buf), "%g", d) : snprintf(buf, sizeof(buf), "%lld", (long long)i);
    n->desc = strdup(buf);
    return (id)n;
}
id ns_number_int(int v) { return number_new(v, 0, 0); }
HM(n_with_int) { return RET_ID(number_new((int32_t)a, 0, 0)); }
HM(n_with_bool) { return RET_ID(number_new(a & 0xFF ? 1 : 0, 0, 0)); }
HM(n_with_float) { return RET_ID(number_new(0, ARG_F(a), 1)); }
HM(n_with_double) { return RET_ID(number_new(0, ARG_D(a, b), 1)); }
HM(n_int) { return (uint32_t)((ns_number_t *)self)->i; }
HM(n_ll) { return (uint64_t)((ns_number_t *)self)->i; }
HM(n_float) { return RET_F((float)((ns_number_t *)self)->d); }
HM(n_double) { return RET_D(((ns_number_t *)self)->d); }
HM(n_is_equal) {
    return a && objc_is_kind((id)a, c_NSNumber) && ((ns_number_t *)a)->d == ((ns_number_t *)self)->d;
}
HM(n_description) { return RET_ID(ns_string(((ns_number_t *)self)->desc)); }

// ---------------------------------------------------------------- NSArray / NSSet

typedef struct {
    objc_class *isa;
    uint32_t count, cap;
    id *items;
} ns_array_t;

static uint32_t mutations;

id ns_array_new(void) {
    ns_array_t *a = calloc(1, sizeof(*a));
    a->isa = c_NSMutableArray;
    return (id)a;
}
void ns_array_add(id arr, id obj) {
    ns_array_t *a = (ns_array_t *)arr;
    if (a->count == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->items = realloc(a->items, a->cap * sizeof(id));
    }
    a->items[a->count++] = obj;
}
static ns_array_t *arr_target(id self) {
    if (!is_meta(self->isa))
        return (ns_array_t *)self;
    ns_array_t *a = calloc(1, sizeof(*a));
    a->isa = (objc_class *)self;
    return a;
}
HM(a_array) { return RET_ID(arr_target(self)); }
HM(a_with_array) {
    ns_array_t *r = arr_target(self), *src = (ns_array_t *)a;
    for (uint32_t i = 0; src && i < src->count; i++)
        ns_array_add((id)r, src->items[i]);
    return RET_ID(r);
}
HM(a_with_object) {
    ns_array_t *r = arr_target(self);
    ns_array_add((id)r, (id)a);
    return RET_ID(r);
}
static uint64_t a_with_objects(id self, SEL sel, uint32_t first, ...) {
    ns_array_t *r = arr_target(self);
    va_list ap;
    va_start(ap, first);
    for (id o = (id)first; o; o = va_arg(ap, id))
        ns_array_add((id)r, o);
    va_end(ap);
    return RET_ID(r);
}
HM(a_count) { return ((ns_array_t *)self)->count; }
HM(a_object_at) {
    ns_array_t *arr = (ns_array_t *)self;
    if (a >= arr->count) {
        l_error("-[%s objectAtIndex:%u] out of range (count %u)", objc_class_name(self), a, arr->count);
        return 0;
    }
    return RET_ID(arr->items[a]);
}
HM(a_last) {
    ns_array_t *arr = (ns_array_t *)self;
    return RET_ID(arr->count ? arr->items[arr->count - 1] : NULL);
}
HM(a_any) {
    ns_array_t *arr = (ns_array_t *)self;
    return RET_ID(arr->count ? arr->items[0] : NULL);
}
HM(a_index_of) {
    ns_array_t *arr = (ns_array_t *)self;
    for (uint32_t i = 0; i < arr->count; i++)
        if (arr->items[i] == (id)a || objc_send(arr->items[i], "isEqual:", a, 0, 0, 0))
            return i;
    return 0x7FFFFFFF;
}
HM(a_contains) { return objc_send(self, "indexOfObject:", a, 0, 0, 0) != 0x7FFFFFFF; }
HM(a_add) {
    ns_array_add(self, (id)a);
    mutations++;
    return 0;
}
HM(a_insert_at) {
    ns_array_t *arr = (ns_array_t *)self;
    ns_array_add(self, NULL);
    if (b > arr->count - 1)
        b = arr->count - 1;
    memmove(&arr->items[b + 1], &arr->items[b], (arr->count - 1 - b) * sizeof(id));
    arr->items[b] = (id)a;
    mutations++;
    return 0;
}
HM(a_remove_at) {
    ns_array_t *arr = (ns_array_t *)self;
    if (a < arr->count) {
        memmove(&arr->items[a], &arr->items[a + 1], (arr->count - a - 1) * sizeof(id));
        arr->count--;
        mutations++;
    }
    return 0;
}
HM(a_remove) {
    ns_array_t *arr = (ns_array_t *)self;
    for (uint32_t i = arr->count; i-- > 0;)
        if (arr->items[i] == (id)a)
            a_remove_at(self, sel, i, 0, 0, 0, 0, 0, 0, 0);
    return 0;
}
HM(a_remove_all) {
    ((ns_array_t *)self)->count = 0;
    mutations++;
    return 0;
}
HM(a_remove_last) {
    ns_array_t *arr = (ns_array_t *)self;
    if (arr->count)
        arr->count--;
    return 0;
}
HM(a_copy) { return a_with_array((id)self->isa, sel, (uint32_t)self, 0, 0, 0, 0, 0, 0, 0); }
// NSFastEnumerationState {state, itemsPtr, mutationsPtr, extra[5]}: one batch with every item.
HM(a_enumerate) {
    uint32_t *st = (uint32_t *)a;
    ns_array_t *arr = (ns_array_t *)self;
    if (st[0])
        return 0;
    st[0] = 1;
    st[1] = (uint32_t)arr->items;
    st[2] = (uint32_t)&mutations;
    return arr->count;
}
HM(a_object_enum);

// ---------------------------------------------------------------- NSDictionary

typedef struct {
    objc_class *isa;
    uint32_t count, cap;
    id *keys, *vals;
} ns_dict_t;

static int key_eq(id a, id b) {
    if (a == b)
        return 1;
    if (is_string(a) && is_string(b))
        return !strcmp(ns_cstr(a), ns_cstr(b));
    return a && b && objc_send(a, "isEqual:", (uint32_t)b, 0, 0, 0);
}
static void dict_set(ns_dict_t *d, id k, id v) {
    for (uint32_t i = 0; i < d->count; i++)
        if (key_eq(d->keys[i], k)) {
            d->vals[i] = v;
            return;
        }
    if (d->count == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->keys = realloc(d->keys, d->cap * sizeof(id));
        d->vals = realloc(d->vals, d->cap * sizeof(id));
    }
    d->keys[d->count] = k;
    d->vals[d->count++] = v;
}
static id dict_get(ns_dict_t *d, id k) {
    for (uint32_t i = 0; d && i < d->count; i++)
        if (key_eq(d->keys[i], k))
            return d->vals[i];
    return NULL;
}
static ns_dict_t *dict_target(id self) {
    if (!is_meta(self->isa))
        return (ns_dict_t *)self;
    ns_dict_t *d = calloc(1, sizeof(*d));
    d->isa = (objc_class *)self;
    return d;
}
HM(d_dict) { return RET_ID(dict_target(self)); }
static uint64_t d_with_objects_keys(id self, SEL sel, uint32_t first, ...) {
    ns_dict_t *d = dict_target(self);
    va_list ap;
    va_start(ap, first);
    for (id v = (id)first; v; v = va_arg(ap, id))
        dict_set(d, va_arg(ap, id), v);
    va_end(ap);
    return RET_ID(d);
}
HM(d_with_object_key) {
    ns_dict_t *d = dict_target(self);
    dict_set(d, (id)b, (id)a);
    return RET_ID(d);
}
HM(d_object_for_key) { return RET_ID(dict_get((ns_dict_t *)self, (id)a)); }
HM(d_set_object) {
    dict_set((ns_dict_t *)self, (id)b, (id)a);
    return 0;
}
HM(d_remove) {
    ns_dict_t *d = (ns_dict_t *)self;
    for (uint32_t i = 0; i < d->count; i++)
        if (key_eq(d->keys[i], (id)a)) {
            d->keys[i] = d->keys[--d->count];
            d->vals[i] = d->vals[d->count];
            return 0;
        }
    return 0;
}
HM(d_count) { return ((ns_dict_t *)self)->count; }
HM(d_all_keys) {
    id arr = ns_array_new();
    for (uint32_t i = 0; i < ((ns_dict_t *)self)->count; i++)
        ns_array_add(arr, ((ns_dict_t *)self)->keys[i]);
    return RET_ID(arr);
}
HM(d_enumerate) {
    uint32_t *st = (uint32_t *)a;
    if (st[0])
        return 0;
    st[0] = 1;
    st[1] = (uint32_t)((ns_dict_t *)self)->keys;
    st[2] = (uint32_t)&mutations;
    return ((ns_dict_t *)self)->count;
}

// ---------------------------------------------------------------- NSData

typedef struct {
    objc_class *isa;
    uint32_t len, cap;
    uint8_t *bytes;
} ns_data_t;

static ns_data_t *data_target(id self) {
    if (!is_meta(self->isa))
        return (ns_data_t *)self;
    ns_data_t *d = calloc(1, sizeof(*d));
    d->isa = (objc_class *)self;
    return d;
}
static void data_append(ns_data_t *d, const void *p, uint32_t n) {
    if (d->len + n > d->cap) {
        d->cap = (d->len + n) * 2;
        d->bytes = realloc(d->bytes, d->cap);
    }
    if (p)
        memcpy(d->bytes + d->len, p, n);
    else
        memset(d->bytes + d->len, 0, n);
    d->len += n;
}
static id data_new(const void *p, uint32_t n) {
    ns_data_t *d = data_target((id)c_NSData);
    data_append(d, p, n);
    return (id)d;
}
HM(s_to_data) { return RET_ID(data_new(ns_cstr(self), ns_len(self))); }
HM(dt_data) { return RET_ID(data_target(self)); }
HM(dt_with_bytes) {
    ns_data_t *d = data_target(self);
    data_append(d, (const void *)a, b);
    return RET_ID(d);
}
HM(dt_with_file) {
    long n;
    char *b2 = read_file(ns_cstr((id)a), &n);
    if (!b2)
        return 0;
    ns_data_t *d = data_target(self);
    d->bytes = (uint8_t *)b2;
    d->len = d->cap = n;
    return RET_ID(d);
}
HM(dt_bytes) { return (uint32_t)((ns_data_t *)self)->bytes; }
HM(dt_length) { return ((ns_data_t *)self)->len; }
HM(dt_append) {
    data_append((ns_data_t *)self, ((ns_data_t *)a)->bytes, ((ns_data_t *)a)->len);
    return 0;
}
HM(dt_append_bytes) {
    data_append((ns_data_t *)self, (const void *)a, b);
    return 0;
}
HM(dt_set_length) {
    ns_data_t *d = (ns_data_t *)self;
    if (a > d->len)
        data_append(d, NULL, a - d->len);
    d->len = a;
    return 0;
}
HM(dt_write) { return write_file(ns_cstr((id)a), ((ns_data_t *)self)->bytes, ((ns_data_t *)self)->len); }

// ---------------------------------------------------------------- NSDate / NSDateFormatter

typedef struct {
    objc_class *isa;
    uint32_t pad;
    double t; // seconds since 1970
} ns_date_t;

static double now_seconds(void) { return sceKernelGetProcessTimeWide() / 1e6 + (double)time(NULL); }
HM(date_now) {
    ns_date_t *d = (ns_date_t *)objc_alloc(c_NSDate);
    d->t = (double)time(NULL);
    return RET_ID(d);
}
HM(date_since_1970) { return RET_D(((ns_date_t *)self)->t); }
HM(date_since_now) { return RET_D(((ns_date_t *)self)->t - (double)time(NULL)); }
HM(df_set_format) {
    ((id *)self)[1] = (id)a; // UIView-free host object: format string at +4
    return 0;
}
HM(df_string_from_date) {
    char out[128];
    time_t t = a ? (time_t)((ns_date_t *)a)->t : time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &tm); // ponytail: ignores the Unicode date pattern
    return RET_ID(ns_string(out));
}

// ---------------------------------------------------------------- NSBundle / paths / locale / defaults

static id bundle_obj;
HM(b_main) { return RET_ID(bundle_obj ? bundle_obj : (bundle_obj = objc_alloc((objc_class *)self))); }
static id path_no_slash(const char *p) {
    char b[256];
    snprintf(b, sizeof(b), "%s", p);
    size_t n = strlen(b);
    if (n && b[n - 1] == '/')
        b[n - 1] = 0;
    return ns_string(b);
}
HM(b_resource_path) { return RET_ID(path_no_slash(BUNDLE_PATH)); }
HM(b_executable_path) { return RET_ID(ns_string(BUNDLE_PATH "ZombieInfection")); }
// pathForResource:ofType:(inDirectory:): nil when the file is missing, like NSBundle.
static id bundle_path(const char *dir, id name, id type) {
    char p[512];
    const char *ext = type ? ns_cstr(type) : "";
    snprintf(p, sizeof(p), BUNDLE_PATH "%s%s%s%s%s", dir, dir[0] ? "/" : "", ns_cstr(name), ext[0] ? "." : "", ext);
    SceIoStat st;
    if (sceIoGetstat(p, &st) < 0) {
        l_debug("pathForResource: %s missing", p);
        return NULL;
    }
    return ns_string(p);
}
HM(b_path_for) { return RET_ID(bundle_path("", (id)a, (id)b)); }
HM(b_path_for_dir) { return RET_ID(bundle_path(c ? ns_cstr((id)c) : "", (id)a, (id)b)); }
HM(b_info_value) {
    const char *k = ns_cstr((id)a);
    if (!strcmp(k, "CFBundleVersion") || !strcmp(k, "CFBundleShortVersionString"))
        return RET_ID(ns_string("1.0.0"));
    if (!strcmp(k, "CFBundleIdentifier"))
        return RET_ID(ns_string("com.gameloft.ZombieInfection"));
    return 0;
}

static id locale_obj;
HM(loc_current) { return RET_ID(locale_obj ? locale_obj : (locale_obj = objc_alloc((objc_class *)self))); }
HM(loc_object_for_key) { return RET_ID(ns_string("US")); }
HM(loc_preferred_languages) {
    id arr = ns_array_new();
    ns_array_add(arr, ns_string("en"));
    return RET_ID(arr);
}

static id defaults_obj;
HM(ud_standard) {
    if (!defaults_obj) {
        defaults_obj = (id)dict_target((id)c_NSDictionary);
    }
    return RET_ID(defaults_obj);
}
HM(ud_integer) { return (uint32_t)objc_send(dict_get((ns_dict_t *)self, (id)a), "intValue", 0, 0, 0, 0); }
HM(ud_set_integer) {
    dict_set((ns_dict_t *)self, (id)b, ns_number_int(a));
    return 0;
}

// ---------------------------------------------------------------- NSFileManager / NSFileHandle

static id fm_obj;
HM(fm_default) { return RET_ID(fm_obj ? fm_obj : (fm_obj = objc_alloc((objc_class *)self))); }
HM(fm_exists_dir) {
    char p[512];
    SceIoStat st;
    int ok = sceIoGetstat(darwin_path(ns_cstr((id)a), p, sizeof(p)), &st) >= 0;
    if (ok && b)
        *(uint8_t *)b = SCE_S_ISDIR(st.st_mode);
    l_debug("fileExistsAtPath(%s) -> %d", p, ok);
    return ok;
}
HM(fm_exists) { return fm_exists_dir(self, sel, a, 0, 0, 0, 0, 0, 0, 0); }
HM(fm_mkdir) {
    char p[512];
    darwin_path(ns_cstr((id)a), p, sizeof(p));
    for (char *s = strchr(p + 5, '/'); s; s = strchr(s + 1, '/')) { // intermediate directories
        *s = 0;
        sceIoMkdir(p, 0777);
        *s = '/';
    }
    int r = sceIoMkdir(p, 0777);
    return r >= 0 || r == (int)0x80010011; // already exists
}
HM(fm_create_file) {
    ns_data_t *d = (ns_data_t *)b;
    return write_file(ns_cstr((id)a), d ? d->bytes : NULL, d ? d->len : 0);
}
HM(fm_remove) {
    char p[512];
    darwin_path(ns_cstr((id)a), p, sizeof(p));
    return sceIoRemove(p) >= 0 || sceIoRmdir(p) >= 0;
}
HM(fm_copy) {
    long n;
    char *buf = read_file(ns_cstr((id)a), &n);
    if (!buf)
        return 0;
    int r = write_file(ns_cstr((id)b), buf, n);
    free(buf);
    return r;
}
HM(fm_attributes) {
    char p[512];
    SceIoStat st;
    if (sceIoGetstat(darwin_path(ns_cstr((id)a), p, sizeof(p)), &st) < 0)
        return 0;
    ns_dict_t *d = dict_target((id)c_NSDictionary);
    ns_date_t *date = (ns_date_t *)objc_alloc(c_NSDate);
    date->t = st.st_mtime.year * 31556952.0 + st.st_mtime.month * 2629746.0 + st.st_mtime.day * 86400.0 +
              st.st_mtime.hour * 3600.0 + st.st_mtime.minute * 60.0 + st.st_mtime.second; // monotonic, not epoch
    dict_set(d, ns_string("NSFileModificationDate"), (id)date);
    dict_set(d, ns_string("NSFileSize"), number_new(st.st_size, 0, 0));
    return RET_ID(d);
}
// enumeratorAtPath: lists the directory up front (non-recursive).
typedef struct {
    objc_class *isa;
    id items;
    uint32_t next;
} ns_enum_t;
HM(fm_enumerator) {
    char p[512];
    ns_enum_t *e = (ns_enum_t *)objc_alloc(c_NSDirEnum);
    e->items = ns_array_new();
    SceUID d = sceIoDopen(darwin_path(ns_cstr((id)a), p, sizeof(p)));
    SceIoDirent ent;
    while (d >= 0 && sceIoDread(d, &ent) > 0)
        ns_array_add(e->items, ns_string(ent.d_name));
    if (d >= 0)
        sceIoDclose(d);
    return RET_ID(e);
}
HM(enum_next) {
    ns_enum_t *e = (ns_enum_t *)self;
    ns_array_t *arr = (ns_array_t *)e->items;
    return RET_ID(e->next < arr->count ? arr->items[e->next++] : NULL);
}
HM(a_object_enum) {
    ns_enum_t *e = (ns_enum_t *)objc_alloc(c_NSDirEnum);
    e->items = self;
    return RET_ID(e);
}

typedef struct {
    objc_class *isa;
    FILE *f;
} ns_fh_t;
static id fh_open(const char *path, const char *mode) {
    char p[512];
    FILE *f = fopen(darwin_path(path, p, sizeof(p)), mode);
    l_debug("NSFileHandle %s (%s) -> %p", p, mode, f);
    if (!f)
        return NULL;
    ns_fh_t *h = (ns_fh_t *)objc_alloc(c_NSFileHandle);
    h->f = f;
    return (id)h;
}
HM(fh_reading) { return RET_ID(fh_open(ns_cstr((id)a), "rb")); }
HM(fh_writing) { return RET_ID(fh_open(ns_cstr((id)a), "r+b")); }
HM(fh_read) {
    ns_data_t *d = data_target((id)c_NSData);
    data_append(d, NULL, a);
    d->len = fread(d->bytes, 1, a, ((ns_fh_t *)self)->f);
    return RET_ID(d);
}
HM(fh_write) {
    fwrite(((ns_data_t *)a)->bytes, 1, ((ns_data_t *)a)->len, ((ns_fh_t *)self)->f);
    return 0;
}
HM(fh_seek_end) {
    fseek(((ns_fh_t *)self)->f, 0, SEEK_END);
    return ftell(((ns_fh_t *)self)->f);
}
HM(fh_seek) { // seekToFileOffset:(unsigned long long) in r2:r3
    fseek(((ns_fh_t *)self)->f, a, SEEK_SET);
    return 0;
}
HM(fh_offset) { return ftell(((ns_fh_t *)self)->f); }
HM(fh_close) {
    if (((ns_fh_t *)self)->f)
        fclose(((ns_fh_t *)self)->f);
    ((ns_fh_t *)self)->f = NULL;
    return 0;
}

// ---------------------------------------------------------------- NSNotificationCenter

typedef struct {
    id observer;
    SEL sel;
    char name[96];
    id object;
} observer_t;
static observer_t observers[64];
static int nobservers;
static id center_obj;

typedef struct {
    objc_class *isa;
    id name, object, user_info;
} ns_notif_t;

HM(nc_default) { return RET_ID(center_obj ? center_obj : (center_obj = objc_alloc((objc_class *)self))); }
HM(nc_add) {
    if (nobservers < 64) {
        observer_t *o = &observers[nobservers++];
        *o = (observer_t){(id)a, (SEL)b, "", (id)a3};
        snprintf(o->name, sizeof(o->name), "%s", c ? ns_cstr((id)c) : "");
        l_debug("NSNotificationCenter: -[%s %s] observes '%s'", objc_class_name((id)a), (SEL)b, o->name);
    }
    return 0;
}
HM(nc_remove) {
    for (int i = 0; i < nobservers; i++)
        if (observers[i].observer == (id)a && (!b || !strcmp(observers[i].name, ns_cstr((id)b))))
            observers[i--] = observers[--nobservers];
    return 0;
}
void ns_post_notification(const char *name, id object) {
    ns_notif_t *n = (ns_notif_t *)objc_alloc(c_NSNotification);
    n->name = ns_string(name);
    n->object = object;
    l_info("NSNotification: %s", name);
    for (int i = 0; i < nobservers; i++)
        if ((!observers[i].name[0] || !strcmp(observers[i].name, name)) &&
            (!observers[i].object || observers[i].object == object))
            objc_send(observers[i].observer, observers[i].sel, (uint32_t)n, 0, 0, 0);
}
HM(nc_post) {
    ns_post_notification(ns_cstr((id)a), (id)b);
    return 0;
}
HM(notif_name) { return RET_ID(((ns_notif_t *)self)->name); }
HM(notif_object) { return RET_ID(((ns_notif_t *)self)->object); }
HM(notif_user_info) { return RET_ID(((ns_notif_t *)self)->user_info); }

// ---------------------------------------------------------------- networking (offline)

HM(url_with_string) { return RET_ID(ns_string(ns_cstr((id)a))); } // NSURL is its string
HM(url_absolute) { return RET_ID(self); }
HM(err_description) { return RET_ID(ns_string("The Internet connection appears to be offline.")); }

static uint32_t utf8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    uint32_t c = *s++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= extra ? 0x3F >> extra : 0xFF;
    while (extra-- && (*s & 0xC0) == 0x80)
        c = (c << 6) | (*s++ & 0x3F);
    *p = s;
    return c;
}

// ---------------------------------------------------------------- fonts (FreeType)

typedef struct {
    uint32_t key; // size << 24 | codepoint
    int16_t left, top, adv;
    uint16_t w, h;
    uint8_t *bmp;
} glyph;

#define GLYPH_CACHE 4096
static glyph gcache[GLYPH_CACHE];
static FT_Library ft;
static FT_Face faces[4];
static int nfaces = -1;

static void fonts_init(void) {
    // Custom font.ttf checked first, then PS Vita system fonts.
    static const char *paths[] = {DATA_PATH "font.ttf", "sa0:data/font/pvf/ltn0.pvf",
                                  "sa0:data/font/pvf/kr0.pvf", "sa0:data/font/pvf/jpn0.pvf"};
    nfaces = 0;
    if (FT_Init_FreeType(&ft))
        return l_error("FreeType failed to initialize: font rendering disabled");
    for (int i = 0; i < 4; i++)
        if (!FT_New_Face(ft, paths[i], 0, &faces[nfaces])) {
            l_info("loaded font: %s", paths[i]);
            nfaces++;
        }
    if (!nfaces)
        l_error("no fonts loaded: text rendering disabled");
}

static const glyph *get_glyph(int size, uint32_t cp) {
    static glyph tmp;
    uint32_t key = (uint32_t)size << 24 | (cp & 0xFFFFFF);
    uint32_t h = (key * 2654435761u) % GLYPH_CACHE;
    glyph *g = NULL;
    for (int i = 0; i < 16; i++) {
        glyph *c = &gcache[(h + i) % GLYPH_CACHE];
        if (c->key == key)
            return c;
        if (!c->key) {
            g = c;
            break;
        }
    }
    if (!g) { // cache full for this bucket: rasterize directly without caching
        free(tmp.bmp);
        g = &tmp;
    }
    memset(g, 0, sizeof(*g));
    g->key = key;
    if (nfaces < 0)
        fonts_init();
    FT_Face f = NULL;
    for (int i = 0; i < nfaces && !f; i++)
        if (FT_Get_Char_Index(faces[i], cp))
            f = faces[i];
    if (!f) {
        g->adv = cp == ' ' ? size / 3 : size / 2;
        return g;
    }
    FT_Set_Pixel_Sizes(f, 0, size);
    if (FT_Load_Char(f, cp, FT_LOAD_RENDER))
        return g;
    FT_GlyphSlot s = f->glyph;
    g->left = s->bitmap_left;
    g->top = s->bitmap_top;
    g->adv = s->advance.x >> 6;
    g->w = s->bitmap.width;
    g->h = s->bitmap.rows;
    g->bmp = malloc(g->w * g->h + 1);
    for (int y = 0; y < g->h; y++)
        memcpy(g->bmp + y * g->w, s->bitmap.buffer + y * s->bitmap.pitch, g->w);
    return g;
}

// Line height and baseline approximations matching iOS 3 Helvetica (14pt->18, 13->16, 11->14).
// Adjust here if vertical alignment requires calibration.
static int line_height(int size) { return size == 14 ? 18 : size == 13 ? 16 : size == 11 ? 14 : (size * 5 + 2) / 4; }
static int baseline(int size) { return (line_height(size) - size) / 2 + (size * 4 + 2) / 5; }

static int text_width(int size, const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;)
        w += get_glyph(size, utf8_next(&p))->adv;
    return w;
}

// ---------------------------------------------------------------- CoreGraphics / UIKit

typedef struct {
    uint8_t *data;
    int w, h, bpr;
    float gray;
} cg_ctx;

static cg_ctx *cur_ctx;

static cg_ctx *cg_BitmapContextCreate(void *data, int w, int h, int bpc, int bpr, void *cs, int info) {
    cg_ctx *c = calloc(1, sizeof(*c));
    *c = (cg_ctx){data, w, h, bpr, 1.0f};
    return c;
}
static void cg_ContextRelease(cg_ctx *c) {
    if (cur_ctx == c)
        cur_ctx = NULL;
    free(c);
}
SOFTFP static void cg_SetGrayFillColor(cg_ctx *c, float gray, float alpha) { c->gray = gray * alpha; }
static void *cg_ColorSpaceCreate(void) { return (void *)1; }
static void cg_noop(void) {}
static void ui_PushContext(cg_ctx *c) { cur_ctx = c; }
static void ui_PopContext(void) { cur_ctx = NULL; }

typedef struct {
    uint32_t tag;
    int size;
} ui_font;

static ui_font *font_of_size(float pt) {
    static ui_font fonts[32];
    int size = (int)pt;
    for (int i = 0; i < 32; i++)
        if (!fonts[i].tag || fonts[i].size == size) {
            fonts[i] = (ui_font){0x464F4E54, size};
            return &fonts[i];
        }
    return &fonts[31];
}

// UIKit draws with Y-axis pointing downwards; atlas context is un-inverted,
// placing text upside-down in memory (glyph row j -> row h-1-j), as expected by guest quad blitters.
static void draw_text(cg_ctx *c, const char *s, int x, int y, int size) {
    int pen = x, base = baseline(size);
    uint8_t ink = (uint8_t)(c->gray * 255.0f);
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        const glyph *g = get_glyph(size, utf8_next(&p));
        for (int gy = 0; gy < g->h; gy++) {
            int row = c->h - 1 - (y + base - g->top + gy);
            if (row < 0 || row >= c->h)
                continue;
            for (int gx = 0; gx < g->w; gx++) {
                int col = pen + g->left + gx;
                if (col < 0 || col >= c->w)
                    continue;
                uint8_t v = g->bmp[gy * g->w + gx] * ink / 255, *d = &c->data[row * c->bpr + col];
                if (v > *d)
                    *d = v;
            }
        }
        pen += g->adv;
    }
}

// ---------------------------------------------------------------- text input (IME)

static int ime_type = -1;
static char ime_utf8[128];
static uint16_t ime_buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1], ime_title[16], ime_initial[1];

static void (*ime_done)(const char *utf8);

void ime_open(int type, void (*done)(const char *utf8)) {
    ime_done = done;
    static const char *titles[] = {"Name", "Number"};
    const char *t = titles[type == 1];
    for (int i = 0; (ime_title[i] = t[i]); i++) {}
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.type = type == 1 ? SCE_IME_TYPE_NUMBER : SCE_IME_TYPE_DEFAULT;
    p.title = ime_title;
    p.maxTextLength = type == 1 ? 4 : 12;
    p.initialText = ime_initial;
    p.inputTextBuffer = ime_buf;
    ime_utf8[0] = 0;
    int r = sceImeDialogInit(&p);
    if (r < 0)
        l_error("sceImeDialogInit = 0x%08X", r);
    ime_type = type; // even if init fails: ime_poll will close dialog event so game doesn't stall
}

int ime_active(void) { return ime_type >= 0; }

void ime_poll(void) {
    if (ime_type < 0)
        return;
    SceCommonDialogStatus st = sceImeDialogGetStatus();
    if (st == SCE_COMMON_DIALOG_STATUS_RUNNING)
        return;
    if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
        SceImeDialogResult res;
        memset(&res, 0, sizeof(res));
        sceImeDialogGetResult(&res);
        if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
            size_t n = 0;
            for (int i = 0; ime_buf[i] && n < sizeof(ime_utf8) - 4; i++) {
                uint16_t c = ime_buf[i];
                if (c < 0x80)
                    ime_utf8[n++] = c;
                else if (c < 0x800)
                    n += sprintf(ime_utf8 + n, "%c%c", 0xC0 | c >> 6, 0x80 | (c & 0x3F));
                else
                    n += sprintf(ime_utf8 + n, "%c%c%c", 0xE0 | c >> 12, 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F));
            }
            ime_utf8[n] = 0;
        }
        sceImeDialogTerm();
    }
    int type = ime_type;
    ime_type = -1;
    l_info("input text entered (%d): \"%s\"", type, ime_utf8);
    if (ime_done)
        ime_done(ime_utf8);
}


// ---------------------------------------------------------------- UIFont / NSString(UIStringDrawing)

HM(f_system_font) { return RET_ID(font_of_size(ARG_F(a))); }
HM(f_font_noop) { return RET_ID(self); }
HM_STRET(s_size_with_font) {
    ui_font *f = (ui_font *)a;
    int size = f && f->tag == 0x464F4E54 ? f->size : 14;
    ((float *)ret)[0] = text_width(size, ns_cstr(self));
    ((float *)ret)[1] = line_height(size);
}
HM_STRET(s_draw_at_point) {
    ui_font *f = (ui_font *)c;
    int size = f && f->tag == 0x464F4E54 ? f->size : 14;
    if (cur_ctx)
        draw_text(cur_ctx, ns_cstr(self), (int)ARG_F(a), (int)ARG_F(b), size);
    ((float *)ret)[0] = text_width(size, ns_cstr(self));
    ((float *)ret)[1] = line_height(size);
}

// ---------------------------------------------------------------- runtime C API

static void objc_set_property(id self, SEL sel, int offset, id value, int atomic, int copy) {
    *(id *)((uint8_t *)self + offset) = copy && is_string(value) ? ns_string(ns_cstr(value)) : value;
}
static void objc_enumeration_mutation(id obj) { l_error("objc: collection %p mutated while enumerated", obj); }
static id objc_begin_catch(void *exc) { return NULL; }
static void objc_noop(void) {}

// NSHomeDirectory() / NSSearchPathForDirectoriesInDomains(): Documents lives in DOCS_PATH.
static id ns_home_directory(void) { return path_no_slash(DATA_PATH); }
static id ns_search_path(int dir, int mask, int expand) {
    id arr = ns_array_new();
    ns_array_add(arr, path_no_slash(dir == 9 /* NSDocumentDirectory */ ? DOCS_PATH : DATA_PATH "Library/Caches/"));
    l_debug("NSSearchPathForDirectoriesInDomains(%d) -> %s", dir, ns_cstr(((ns_array_t *)arr)->items[0]));
    return arr;
}

// CoreFoundation pieces used at launch.
static void *cf_main_bundle(void) { return &bundle_obj; }
static uint32_t cf_bundle_version(void *b) { return 0x01008000; } // 1.0.0 (major<<24 | minor<<20 | bug<<16 | stage)
static id cf_bundle_info_value(void *b, id key) {
    return (id)(uintptr_t)b_info_value(NULL, NULL, (uint32_t)key, 0, 0, 0, 0, 0, 0, 0);
}
static void *cf_null(void) { return NULL; }

// ---------------------------------------------------------------- method tables

static const host_method foundation_methods[] = {
    {"NSObject", '+', "alloc", o_alloc}, {"NSObject", '+', "allocWithZone:", o_alloc},
    {"NSObject", '+', "new", o_new}, {"NSObject", '+', "class", o_self},
    {"NSObject", '+', "instancesRespondToSelector:", o_instances_respond},
    {"NSObject", '+', "initialize", o_void}, {"NSObject", '+', "load", o_void},
    {"NSObject", '-', "init", o_self}, {"NSObject", '-', "retain", o_self},
    {"NSObject", '-', "autorelease", o_self}, {"NSObject", '-', "release", o_void},
    {"NSObject", '-', "dealloc", o_void}, {"NSObject", '-', "drain", o_void},
    {"NSObject", '-', "self", o_self}, {"NSObject", '-', "class", o_class},
    {"NSObject", '-', "respondsToSelector:", o_responds}, {"NSObject", '-', "isKindOfClass:", o_is_kind},
    {"NSObject", '-', "isMemberOfClass:", o_is_member}, {"NSObject", '-', "conformsToProtocol:", o_yes},
    {"NSObject", '-', "isEqual:", o_is_equal}, {"NSObject", '-', "hash", o_hash},
    {"NSObject", '-', "retainCount", o_retain_count}, {"NSObject", '-', "description", o_description},
    {"NSObject", '-', "copy", o_self}, {"NSObject", '-', "mutableCopy", o_self},
    {"NSObject", '-', "performSelector:", o_perform0}, {"NSObject", '-', "performSelector:withObject:", o_perform1},
    {"NSObject", '-', "performSelector:withObject:withObject:", o_perform2},
    {"NSObject", '-', "performSelectorOnMainThread:withObject:waitUntilDone:", o_perform_main},
    {"NSThread", '+', "detachNewThreadSelector:toTarget:withObject:", thread_detach},
    {"NSThread", '+', "isMainThread", o_yes}, {"NSThread", '-', "isMainThread", o_yes},

    {"NSString", '+', "string", s_empty}, {"NSString", '+', "stringWithUTF8String:", s_with_cstr},
    {"NSString", '+', "stringWithCString:encoding:", s_with_cstr}, {"NSString", '+', "stringWithCString:", s_with_cstr},
    {"NSString", '+', "stringWithString:", s_with_string}, {"NSString", '+', "stringWithFormat:", s_with_format},
    {"NSString", '+', "stringWithContentsOfFile:", s_with_file},
    {"NSString", '+', "stringWithContentsOfFile:encoding:error:", s_with_file},
    {"NSString", '-', "init", s_empty}, {"NSString", '-', "initWithUTF8String:", s_with_cstr},
    {"NSString", '-', "initWithCString:encoding:", s_with_cstr}, {"NSString", '-', "initWithCString:", s_with_cstr},
    {"NSString", '-', "initWithString:", s_with_string}, {"NSString", '-', "initWithFormat:", s_with_format},
    {"NSString", '-', "initWithBytes:length:encoding:", s_with_bytes},
    {"NSString", '-', "initWithData:encoding:", s_with_data}, {"NSString", '-', "initWithContentsOfFile:", s_with_file},
    {"NSString", '-', "initWithContentsOfFile:encoding:error:", s_with_file},
    {"NSString", '-', "UTF8String", s_utf8}, {"NSString", '-', "cString", s_utf8},
    {"NSString", '-', "cStringUsingEncoding:", s_utf8}, {"NSString", '-', "description", o_self},
    {"NSString", '-', "getCString:maxLength:encoding:", s_get_cstring}, {"NSString", '-', "length", s_length},
    {"NSString", '-', "characterAtIndex:", s_char_at}, {"NSString", '-', "isEqualToString:", s_is_equal},
    {"NSString", '-', "isEqual:", s_is_equal}, {"NSString", '-', "hash", s_hash},
    {"NSString", '-', "compare:", s_compare}, {"NSString", '-', "caseInsensitiveCompare:", s_case_compare},
    {"NSString", '-', "hasPrefix:", s_has_prefix}, {"NSString", '-', "hasSuffix:", s_has_suffix},
    {"NSString", '-', "stringByAppendingString:", s_append}, {"NSString", '-', "stringByAppendingFormat:", s_append_format},
    {"NSString", '-', "stringByAppendingPathComponent:", s_append_path},
    {"NSString", '-', "lastPathComponent", s_last_path}, {"NSString", '-', "pathExtension", s_path_ext},
    {"NSString", '-', "stringByDeletingLastPathComponent", s_delete_last_path},
    {"NSString", '-', "stringByDeletingPathExtension", s_delete_ext},
    {"NSString", '-', "isAbsolutePath", s_is_absolute},
    {"NSString", '-', "stringByReplacingOccurrencesOfString:withString:", s_replace},
    {"NSString", '-', "substringToIndex:", s_substr_to}, {"NSString", '-', "substringFromIndex:", s_substr_from},
    {"NSString", '-', "substringWithRange:", s_substr_range}, {"NSString", '-', "rangeOfString:", s_range_of},
    {"NSString", '-', "rangeOfString:options:", s_range_of},
    {"NSString", '-', "componentsSeparatedByString:", s_components}, {"NSString", '-', "intValue", s_int_value},
    {"NSString", '-', "integerValue", s_int_value}, {"NSString", '-', "floatValue", s_float_value},
    {"NSString", '-', "doubleValue", s_double_value}, {"NSString", '-', "boolValue", s_bool_value},
    {"NSString", '-', "lowercaseString", s_lower}, {"NSString", '-', "uppercaseString", s_upper},
    {"NSString", '-', "copy", s_copy}, {"NSString", '-', "mutableCopy", s_copy},
    {"NSString", '-', "dataUsingEncoding:", s_to_data}, {"NSString", '-', "dataUsingEncoding:allowLossyConversion:", s_to_data},
    {"NSString", '-', "writeToFile:atomically:", s_write}, {"NSString", '-', "writeToFile:atomically:encoding:error:", s_write},
    {"NSString", '-', "sizeWithFont:", s_size_with_font}, {"NSString", '-', "drawAtPoint:withFont:", s_draw_at_point},

    {"NSNumber", '+', "numberWithInt:", n_with_int}, {"NSNumber", '+', "numberWithInteger:", n_with_int},
    {"NSNumber", '+', "numberWithUnsignedInt:", n_with_int}, {"NSNumber", '+', "numberWithBool:", n_with_bool},
    {"NSNumber", '+', "numberWithFloat:", n_with_float}, {"NSNumber", '+', "numberWithDouble:", n_with_double},
    {"NSNumber", '-', "intValue", n_int}, {"NSNumber", '-', "integerValue", n_int},
    {"NSNumber", '-', "unsignedIntValue", n_int}, {"NSNumber", '-', "boolValue", n_int},
    {"NSNumber", '-', "longLongValue", n_ll}, {"NSNumber", '-', "floatValue", n_float},
    {"NSNumber", '-', "doubleValue", n_double}, {"NSNumber", '-', "isEqual:", n_is_equal},
    {"NSNumber", '-', "isEqualToNumber:", n_is_equal}, {"NSNumber", '-', "description", n_description},
    {"NSNumber", '-', "stringValue", n_description},

    {"NSArray", '+', "array", a_array}, {"NSArray", '+', "arrayWithArray:", a_with_array},
    {"NSArray", '+', "arrayWithObject:", a_with_object}, {"NSArray", '+', "arrayWithObjects:", a_with_objects},
    {"NSArray", '-', "init", a_array}, {"NSArray", '-', "initWithArray:", a_with_array},
    {"NSArray", '-', "initWithObjects:", a_with_objects}, {"NSArray", '-', "initWithCapacity:", a_array},
    {"NSArray", '+', "arrayWithCapacity:", a_array},
    {"NSArray", '-', "count", a_count}, {"NSArray", '-', "objectAtIndex:", a_object_at},
    {"NSArray", '-', "lastObject", a_last}, {"NSArray", '-', "anyObject", a_any},
    {"NSArray", '-', "allObjects", o_self}, {"NSArray", '-', "indexOfObject:", a_index_of},
    {"NSArray", '-', "containsObject:", a_contains}, {"NSArray", '-', "objectEnumerator", a_object_enum},
    {"NSArray", '-', "countByEnumeratingWithState:objects:count:", a_enumerate},
    {"NSArray", '-', "copy", a_copy}, {"NSArray", '-', "mutableCopy", a_copy},
    {"NSArray", '-', "addObject:", a_add}, {"NSArray", '-', "insertObject:atIndex:", a_insert_at},
    {"NSArray", '-', "removeObjectAtIndex:", a_remove_at}, {"NSArray", '-', "removeObject:", a_remove},
    {"NSArray", '-', "removeAllObjects", a_remove_all}, {"NSArray", '-', "removeLastObject", a_remove_last},

    {"NSDictionary", '+', "dictionary", d_dict}, {"NSDictionary", '+', "dictionaryWithObjectsAndKeys:", d_with_objects_keys},
    {"NSDictionary", '+', "dictionaryWithObject:forKey:", d_with_object_key},
    {"NSDictionary", '+', "dictionaryWithCapacity:", d_dict},
    {"NSDictionary", '-', "init", d_dict}, {"NSDictionary", '-', "initWithObjectsAndKeys:", d_with_objects_keys},
    {"NSDictionary", '-', "initWithCapacity:", d_dict},
    {"NSDictionary", '-', "objectForKey:", d_object_for_key}, {"NSDictionary", '-', "valueForKey:", d_object_for_key},
    {"NSDictionary", '-', "setObject:forKey:", d_set_object}, {"NSDictionary", '-', "setValue:forKey:", d_set_object},
    {"NSDictionary", '-', "removeObjectForKey:", d_remove}, {"NSDictionary", '-', "count", d_count},
    {"NSDictionary", '-', "allKeys", d_all_keys},
    {"NSDictionary", '-', "countByEnumeratingWithState:objects:count:", d_enumerate},

    {"NSData", '+', "data", dt_data}, {"NSData", '+', "dataWithBytes:length:", dt_with_bytes},
    {"NSData", '+', "dataWithContentsOfFile:", dt_with_file}, {"NSData", '-', "init", dt_data},
    {"NSData", '-', "initWithBytes:length:", dt_with_bytes}, {"NSData", '-', "initWithContentsOfFile:", dt_with_file},
    {"NSData", '-', "initWithContentsOfURL:", dt_with_file}, {"NSData", '-', "bytes", dt_bytes},
    {"NSData", '-', "mutableBytes", dt_bytes}, {"NSData", '-', "length", dt_length},
    {"NSData", '-', "appendData:", dt_append}, {"NSData", '-', "appendBytes:length:", dt_append_bytes},
    {"NSData", '-', "setLength:", dt_set_length}, {"NSData", '-', "writeToFile:atomically:", dt_write},
    {"NSMutableData", '+', "dataWithLength:", dt_data}, {"NSData", '+', "dataWithCapacity:", dt_data},

    {"NSDate", '+', "date", date_now}, {"NSDate", '-', "timeIntervalSince1970", date_since_1970},
    {"NSDate", '-', "timeIntervalSinceNow", date_since_now},
    {"NSDateFormatter", '-', "setDateFormat:", df_set_format},
    {"NSDateFormatter", '-', "stringFromDate:", df_string_from_date},

    {"NSBundle", '+', "mainBundle", b_main}, {"NSBundle", '-', "resourcePath", b_resource_path},
    {"NSBundle", '-', "bundlePath", b_resource_path}, {"NSBundle", '-', "executablePath", b_executable_path},
    {"NSBundle", '-', "pathForResource:ofType:", b_path_for},
    {"NSBundle", '-', "pathForResource:ofType:inDirectory:", b_path_for_dir},
    {"NSBundle", '-', "objectForInfoDictionaryKey:", b_info_value},
    {"NSLocale", '+', "currentLocale", loc_current}, {"NSLocale", '+', "preferredLanguages", loc_preferred_languages},
    {"NSLocale", '-', "objectForKey:", loc_object_for_key},
    {"NSUserDefaults", '+', "standardUserDefaults", ud_standard},
    {"NSDictionary", '-', "integerForKey:", ud_integer}, {"NSDictionary", '-', "boolForKey:", ud_integer},
    {"NSDictionary", '-', "setInteger:forKey:", ud_set_integer}, {"NSDictionary", '-', "setBool:forKey:", ud_set_integer},
    {"NSDictionary", '-', "synchronize", o_yes},

    {"NSFileManager", '+', "defaultManager", fm_default}, {"NSFileManager", '-', "fileExistsAtPath:", fm_exists},
    {"NSFileManager", '-', "fileExistsAtPath:isDirectory:", fm_exists_dir},
    {"NSFileManager", '-', "createDirectoryAtPath:attributes:", fm_mkdir},
    {"NSFileManager", '-', "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", fm_mkdir},
    {"NSFileManager", '-', "createFileAtPath:contents:attributes:", fm_create_file},
    {"NSFileManager", '-', "removeItemAtPath:error:", fm_remove},
    {"NSFileManager", '-', "copyItemAtPath:toPath:error:", fm_copy},
    {"NSFileManager", '-', "fileAttributesAtPath:traverseLink:", fm_attributes},
    {"NSFileManager", '-', "attributesOfItemAtPath:error:", fm_attributes},
    {"NSFileManager", '-', "enumeratorAtPath:", fm_enumerator},
    {"NSDirectoryEnumerator", '-', "nextObject", enum_next},
    {"NSFileHandle", '+', "fileHandleForReadingAtPath:", fh_reading},
    {"NSFileHandle", '+', "fileHandleForWritingAtPath:", fh_writing},
    {"NSFileHandle", '+', "fileHandleForUpdatingAtPath:", fh_writing},
    {"NSFileHandle", '-', "readDataOfLength:", fh_read}, {"NSFileHandle", '-', "writeData:", fh_write},
    {"NSFileHandle", '-', "seekToEndOfFile", fh_seek_end}, {"NSFileHandle", '-', "seekToFileOffset:", fh_seek},
    {"NSFileHandle", '-', "offsetInFile", fh_offset}, {"NSFileHandle", '-', "closeFile", fh_close},

    {"NSNotificationCenter", '+', "defaultCenter", nc_default},
    {"NSNotificationCenter", '-', "addObserver:selector:name:object:", nc_add},
    {"NSNotificationCenter", '-', "removeObserver:name:object:", nc_remove},
    {"NSNotificationCenter", '-', "removeObserver:", nc_remove},
    {"NSNotificationCenter", '-', "postNotificationName:object:", nc_post},
    {"NSNotification", '-', "name", notif_name}, {"NSNotification", '-', "object", notif_object},
    {"NSNotification", '-', "userInfo", notif_user_info},

    {"NSURL", '+', "URLWithString:", url_with_string}, {"NSURL", '+', "fileURLWithPath:", url_with_string},
    {"NSString", '-', "absoluteString", url_absolute}, {"NSString", '-', "path", url_absolute},
    {"NSError", '-', "localizedDescription", err_description},
    {"UIFont", '+', "systemFontOfSize:", f_system_font}, {"UIFont", '+', "boldSystemFontOfSize:", f_system_font},
    {"UIFont", '+', "fontWithName:size:", f_system_font},
};

// ---------------------------------------------------------------- imports

// NSString constants exported by Foundation/UIKit/MediaPlayer/EAGL: each import is a variable
// holding an NSString*, whose value is the constant's own name (as on iOS for most of them).
static const char *const_names[] = {
    "NSErrorFailingURLStringKey", "NSFileModificationDate", "NSLocaleCountryCode", "kCFBundleVersionKey",
    "UIKeyboardWillHideNotification", "UIKeyboardWillShowNotification", "UITextFieldTextDidChangeNotification",
    "MPMediaItemPropertyAlbumTitle", "MPMediaItemPropertyArtist", "MPMediaItemPropertyTitle",
    "MPMediaPlaylistPropertyName", "MPMoviePlayerContentPreloadDidFinishNotification",
    "MPMoviePlayerPlaybackDidFinishNotification", "MPMusicPlayerControllerNowPlayingItemDidChangeNotification",
    "MPMusicPlayerControllerPlaybackStateDidChangeNotification", "kEAGLColorFormatRGBA8",
    "kEAGLDrawablePropertyColorFormat", "kEAGLDrawablePropertyRetainedBacking",
};
#define NCONST (sizeof(const_names) / sizeof(const_names[0]))
static id const_values[NCONST];
static uint8_t empty_cache[64];

static const import_entry fixed_imports[] = {
    // Bound directly (data = 1): the asm entries align the stack themselves only when they call our
    // code, so guest IMPs receive the caller's untouched stack.
    {"_objc_msgSend", (uintptr_t)objc_msgSend_entry, 1},
    {"_objc_msgSend_stret", (uintptr_t)objc_msgSend_stret_entry, 1},
    {"_objc_msgSendSuper2", (uintptr_t)objc_msgSendSuper2_entry, 1}, F(objc_setProperty, objc_set_property),
    F(objc_enumerationMutation, objc_enumeration_mutation), F(objc_begin_catch, objc_begin_catch),
    F(objc_end_catch, objc_noop), {"__objc_empty_cache", (uintptr_t)empty_cache, 1},
    {"__objc_empty_vtable", (uintptr_t)empty_cache, 1},
    F(NSHomeDirectory, ns_home_directory), F(NSSearchPathForDirectoriesInDomains, ns_search_path),
    F(CFBundleGetMainBundle, cf_main_bundle), F(CFBundleGetVersionNumber, cf_bundle_version),
    F(CFBundleGetValueForInfoDictionaryKey, cf_bundle_info_value), F(CFRelease, objc_noop),
    F(CFAllocatorGetDefault, cf_null),
    F(CGBitmapContextCreate, cg_BitmapContextCreate), F(CGContextRelease, cg_ContextRelease),
    F(CGContextSetGrayFillColor, cg_SetGrayFillColor), F(CGContextSetTextDrawingMode, cg_noop),
    F(CGColorSpaceCreateDeviceGray, cg_ColorSpaceCreate), F(CGColorSpaceCreateDeviceRGB, cg_ColorSpaceCreate),
    F(CGColorSpaceRelease, cg_noop), F(UIGraphicsPushContext, ui_PushContext),
    F(UIGraphicsPopContext, ui_PopContext),
};
const import_entry objc_imports[] = {{"", 0, 0}}; // classes/constants are resolved by objc_import_lookup
const int objc_imports_count = 0;

// _OBJC_CLASS_$_X / _OBJC_METACLASS_$_X / constants, created on demand while the loader binds imports.
const import_entry *objc_import_lookup(const char *name) {
    static import_entry e;
    for (unsigned i = 0; i < sizeof(fixed_imports) / sizeof(fixed_imports[0]); i++)
        if (!strcmp(fixed_imports[i].name, name))
            return &fixed_imports[i];
    if (!strncmp(name, "_OBJC_CLASS_$_", 14)) {
        e = (import_entry){name, (uintptr_t)&host_get(name + 14)->cls, 1};
        return &e;
    }
    if (!strncmp(name, "_OBJC_METACLASS_$_", 18)) {
        e = (import_entry){name, (uintptr_t)&host_get(name + 18)->meta, 1};
        return &e;
    }
    if (!strcmp(name, "___CFConstantStringClassReference")) {
        e = (import_entry){name, (uintptr_t)c_NSString, 1};
        return &e;
    }
    for (unsigned i = 0; i < NCONST; i++)
        if (!strcmp(name + 1, const_names[i])) {
            e = (import_entry){name, (uintptr_t)&const_values[i], 1};
            return &e;
        }
    return NULL;
}

void objc_init(void) {
    c_NSString = objc_host_class("NSString");
    c_NSArray = objc_host_class("NSArray");
    c_NSMutableArray = objc_host_class("NSMutableArray");
    c_NSNumber = objc_host_class("NSNumber");
    c_NSDictionary = objc_host_class("NSMutableDictionary");
    c_NSData = objc_host_class("NSMutableData");
    c_NSMutableData = c_NSData;
    c_NSDate = objc_host_class("NSDate");
    c_NSFileHandle = objc_host_class("NSFileHandle");
    c_NSDirEnum = objc_host_class("NSDirectoryEnumerator");
    c_NSNotification = objc_host_class("NSNotification");
    for (unsigned i = 0; i < NCONST; i++)
        const_values[i] = ns_string(const_names[i]);
    objc_host_class("NSUserDefaults")->superclass = objc_host_class("NSMutableDictionary");
    objc_host_class("NSUserDefaults")->isa->superclass = objc_host_class("NSMutableDictionary")->isa;
    objc_register(foundation_methods, sizeof(foundation_methods) / sizeof(foundation_methods[0]));
    l_info("objc: runtime ready (%d host classes)", nhosts);
}
