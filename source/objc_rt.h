/**
 * @file objc_rt.h
 * @brief Minimal Objective-C 2.0 runtime (iOS 3, 32-bit ARM) shared by objc.c and uikit.c.
 * @details Guest classes are the compiled ones in __objc_classlist; host classes (NSObject,
 *          UIView, ...) are ours and their methods are C functions registered in host_method
 *          tables. A guest class inheriting from a host class reaches the host methods by walking
 *          the superclass chain, exactly like the real runtime.
 *
 *          Host method ABI (all words, softfp): uint64_t fn(id self, SEL sel, a, b, c, ...).
 *          a/b arrive in r2/r3 and the rest on the stack; doubles occupy two consecutive words
 *          (use ARG_D), floats one (ARG_F). The 64-bit return covers id/int (r0) and double (r0:r1).
 *          Methods returning structures (CGRect, CGPoint, NSRange...) are called through
 *          objc_msgSend_stret: fn(void *ret, id self, SEL sel, a, b, ...).
 */
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct objc_class objc_class;
typedef struct objc_object {
    objc_class *isa;
} objc_object;
typedef objc_object *id;
typedef const char *SEL;

typedef struct {
    const char *name, *types;
    void *imp;
} objc_method;

typedef struct {
    uint32_t entsize, count;
    objc_method m[];
} objc_method_list;

typedef struct {
    uint32_t flags, instanceStart, instanceSize, ivarLayout;
    const char *name;
    objc_method_list *baseMethods;
    void *baseProtocols, *ivars, *weakIvarLayout, *baseProperties;
} objc_class_ro;

struct objc_class {
    objc_class *isa, *superclass;
    void *cache, *vtable;
    objc_class_ro *data;
};

typedef struct {
    const char *cls; // host class name
    char kind;       // '-' instance method, '+' class method
    const char *sel;
    void *fn;
} host_method;

typedef union {
    uint32_t w[2];
    double d;
    float f;
    uint32_t u;
} objc_word;

#define ARG_F(x) (((objc_word){.u = (x)}).f)
#define ARG_D(lo, hi) (((objc_word){.w = {(lo), (hi)}}).d)
#define RET_F(x) ((uint64_t)((objc_word){.f = (x)}).u)
#define RET_D(x) (((objc_word){.d = (x)}).w[0] | (uint64_t)((objc_word){.d = (x)}).w[1] << 32)
#define RET_ID(x) ((uint64_t)(uintptr_t)(x))

// Host method signatures.
#define HM(name) static uint64_t name(id self, SEL sel, uint32_t a, uint32_t b, uint32_t c, uint32_t a3, \
                                      uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
#define HM_STRET(name) static void name(void *ret, id self, SEL sel, uint32_t a, uint32_t b, uint32_t c, \
                                        uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)

typedef struct {
    float x, y;
} CGPoint;
typedef struct {
    float w, h;
} CGSize;
typedef struct {
    CGPoint o;
    CGSize s;
} CGRect;

/** Registers a table of host methods (call before macho_load). */
void objc_register(const host_method *m, int n);
/** Host class object by name (NSObject, UIView, ...), or NULL. */
objc_class *objc_host_class(const char *name);
/** Guest or host class by name (guest classes only after macho_load). */
objc_class *objc_get_class(const char *name);
/** Allocates a zeroed instance of cls (guest instanceSize or host size). */
id objc_alloc(objc_class *cls);
/** Name of the class of obj (or of the class itself if obj is a class). */
const char *objc_class_name(id obj);
/** 1 if obj's class is cls or inherits from it. */
int objc_is_kind(id obj, objc_class *cls);
/** 1 if obj implements sel (guest or host, without the "unknown" fallback). */
int objc_responds(id obj, SEL sel);
/** Sends a message from host code (guest IMPs run through call_guest*). */
uint32_t objc_send(id self, SEL sel, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

// Foundation helpers exposed to uikit.c
id ns_string(const char *s);
const char *ns_cstr(id s);
id ns_array_new(void);
void ns_array_add(id arr, id obj);
id ns_number_int(int v);
void ns_post_notification(const char *name, id object);
