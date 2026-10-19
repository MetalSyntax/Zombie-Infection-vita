/**
 * @file darwin.c
 * @brief iOS Darwin libc, libgcc, and pthread emulation layer over Vita newlib.
 * @details Implements ARMv6 softfp Darwin system calls, POSIX threads with SceUID safety,
 *          virtual filesystem path translation to ux0:data/, high-resolution timers,
 *          and mathematical builtins.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <wchar.h>

#include "darwin_printf.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"

// ---------------------------------------------------------------- paths

// Emulated working directory (Vita processes have none). Irrlicht chdir()s to the bundle and
// opens "data.pack", "*.gt", "*.bsprite" relative to it; the bundle is also the default.
static char cwd[512] = BUNDLE_PATH;

static int is_device_path(const char *p) {
    return !strncmp(p, "ux0:", 4) || !strncmp(p, "ur0:", 4) || !strncmp(p, "app0:", 5);
}

// Paths created by the game are mapped to the Vita: device paths stay, iOS absolute paths
// ("/var/...") go under DATA_PATH and relative ones under the working directory.
// "dir/../" is collapsed (sceIo does not resolve it): the game opens "<bundle>/../Documents/x",
// and the iOS sandbox Documents/ folder is DOCS_PATH.
const char *darwin_path(const char *in, char *out, size_t outlen) {
    char tmp[512];
    if (is_device_path(in)) {
        snprintf(tmp, sizeof(tmp), "%s", in);
    } else if (*in == '/') {
        while (*in == '/')
            in++;
        snprintf(tmp, sizeof(tmp), DATA_PATH "%s", in);
    } else {
        while (in[0] == '.' && in[1] == '/')
            in += 2;
        snprintf(tmp, sizeof(tmp), "%s%s", cwd, in);
    }
    for (char *p; (p = strstr(tmp, "/../"));) {
        char *q = p;
        while (q > tmp && q[-1] != '/' && q[-1] != ':')
            q--;
        memmove(q, p + 4, strlen(p + 4) + 1);
    }
    static const char docs[] = DATA_PATH "Documents";
    size_t dl = sizeof(docs) - 1;
    if (!strncmp(tmp, docs, dl) && (tmp[dl] == '/' || !tmp[dl]))
        snprintf(out, outlen, DOCS_PATH "%s", tmp[dl] ? tmp + dl + 1 : "");
    else
        snprintf(out, outlen, "%s", tmp);
    return out;
}

static FILE *d_fopen(const char *path, const char *mode) {
    char p[512];
    FILE *f = fopen(darwin_path(path, p, sizeof(p)), mode);
    l_debug("fopen(%s, %s) -> %p", p, mode, f);
    return f;
}

static int d_mkdir(const char *path, int mode) {
    char p[512];
    return mkdir(darwin_path(path, p, sizeof(p)), mode);
}

// ---------------------------------------------------------------- stdio / errno / _chk

static FILE *d_stderrp, *d_stdinp;

static int *d___error(void) { return &errno; }

static void *d___memcpy_chk(void *d, const void *s, size_t n, size_t dn) { return memcpy(d, s, n); }
static char *d___strcpy_chk(char *d, const char *s, size_t dn) { return strcpy(d, s); }

// ---------------------------------------------------------------- printf with Darwin ABI (darwin_printf.c)

static int d_sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, INT_MAX, fmt, &w);
    va_end(ap);
    return r;
}
static int d___sprintf_chk(char *buf, int flag, size_t len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, len, fmt, &w);
    va_end(ap);
    return r;
}
static int d_vsprintf(char *buf, const char *fmt, const uint32_t *ap) {
    words w = {NULL, ap};
    return darwin_vformat(buf, INT_MAX, fmt, &w);
}
static int d_vsnprintf(char *buf, size_t size, const char *fmt, const uint32_t *ap) {
    words w = {NULL, ap};
    return darwin_vformat(buf, size, fmt, &w);
}

static void d___assert_rtn(const char *func, const char *file, int line, const char *expr) {
    l_error("guest assertion failed: %s (%s:%d %s)", expr, file, line, func);
}

// NSLog(@"format", ...): format is a CFString (C string pointer at +8).
static void d_NSLog(void *cfstr, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, cfstr);
    words w = {&ap, NULL};
    darwin_vformat(buf, sizeof(buf), cfstr ? *(const char **)((uint8_t *)cfstr + 8) : "(nil)", &w);
    va_end(ap);
    l_info("NSLog: %s", buf);
}

static int qsort_guest_cmp;
static int qsort_tramp(const void *a, const void *b) {
    return (int)call_guest((void *)qsort_guest_cmp, (uint32_t)a, (uint32_t)b, 0, 0);
}
static void d_qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    int prev = qsort_guest_cmp; // nested qsort: restored upon return
    qsort_guest_cmp = (int)cmp;
    qsort(base, n, sz, qsort_tramp);
    qsort_guest_cmp = prev;
}

// ---------------------------------------------------------------- time / system

static uint64_t d_mach_absolute_time(void) { return sceKernelGetProcessTimeWide(); }

typedef struct { uint32_t numer, denom; } mach_timebase_info_t;
static int d_mach_timebase_info(mach_timebase_info_t *info) {
    info->numer = 1000; // mach_absolute_time is in microseconds
    info->denom = 1;
    return 0;
}

static int d_usleep(unsigned us) {
    sceKernelDelayThread(us);
    return 0;
}

static int d_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    if (!strcmp(name, "hw.machine")) {
        const char *model = "iPhone2,1"; // iPhone 3GS: game selects capable GLES1 texture profile
        size_t len = strlen(model) + 1;
        if (oldp && *oldlenp >= len)
            memcpy(oldp, model, len);
        *oldlenp = len;
        return 0;
    }
    errno = ENOENT;
    return -1;
}

static void d_exit(int code) {
    l_info("guest invoked exit(%d)", code);
    sceKernelExitProcess(0);
}

// ---------------------------------------------------------------- Darwin struct layouts

// Darwin 32-bit struct stat (pre-INODE64 `_stat`): 96 bytes, different from newlib's.
typedef struct {
    int32_t st_dev;
    uint32_t st_ino;
    uint16_t st_mode, st_nlink;
    uint32_t st_uid, st_gid;
    int32_t st_rdev;
    int32_t st_atime_, st_atime_ns, st_mtime_, st_mtime_ns, st_ctime_, st_ctime_ns;
    int64_t st_size, st_blocks;
    int32_t st_blksize;
    uint32_t st_flags, st_gen, st_lspare;
    int64_t st_qspare[2];
} darwin_stat;

static int d_stat(const char *path, darwin_stat *out) {
    char p[512];
    struct stat st;
    int r = stat(darwin_path(path, p, sizeof(p)), &st);
    l_debug("stat(%s) -> %d", p, r);
    if (r < 0)
        return r;
    memset(out, 0, sizeof(*out));
    out->st_mode = st.st_mode; // S_IF* / permission bits are identical
    out->st_nlink = 1;
    out->st_size = st.st_size;
    out->st_blocks = (st.st_size + 511) / 512;
    out->st_blksize = 4096;
    out->st_atime_ = st.st_atime;
    out->st_mtime_ = st.st_mtime;
    out->st_ctime_ = st.st_ctime;
    return 0;
}

// Darwin pre-INODE64 struct dirent: {ino32, reclen16, type8, namlen8, name[256]}.
typedef struct {
    uint32_t d_ino;
    uint16_t d_reclen;
    uint8_t d_type, d_namlen;
    char d_name[256];
} darwin_dirent;
typedef struct {
    DIR *dir;
    darwin_dirent ent;
} darwin_dir;

static darwin_dir *d_opendir(const char *path) {
    char p[512];
    DIR *d = opendir(darwin_path(path, p, sizeof(p)));
    l_debug("opendir(%s) -> %p", p, d);
    if (!d)
        return NULL;
    darwin_dir *dd = calloc(1, sizeof(*dd));
    dd->dir = d;
    return dd;
}
static darwin_dirent *d_readdir(darwin_dir *dd) {
    struct dirent *e = readdir(dd->dir);
    if (!e)
        return NULL;
    snprintf(dd->ent.d_name, sizeof(dd->ent.d_name), "%s", e->d_name);
    dd->ent.d_namlen = strlen(dd->ent.d_name);
    dd->ent.d_reclen = sizeof(dd->ent);
    dd->ent.d_ino = 1;
    dd->ent.d_type = SCE_S_ISDIR(e->d_stat.st_mode) ? 4 /* DT_DIR */ : 8 /* DT_REG */;
    return &dd->ent;
}
static int d_closedir(darwin_dir *dd) {
    int r = closedir(dd->dir);
    free(dd);
    return r;
}

static char *d_realpath(const char *path, char *resolved) {
    char p[512];
    darwin_path(path, p, sizeof(p));
    if (!resolved)
        return strdup(p);
    snprintf(resolved, 1024, "%s", p); // Darwin PATH_MAX
    return resolved;
}
static int d_chdir(const char *path) {
    char p[sizeof(cwd) - 1];
    darwin_path(path, p, sizeof(p));
    SceIoStat st;
    if (sceIoGetstat(p, &st) < 0 || !SCE_S_ISDIR(st.st_mode)) {
        l_warn("chdir(%s): not a directory", p);
        errno = ENOENT;
        return -1;
    }
    size_t n = strlen(p);
    snprintf(cwd, sizeof(cwd), "%s%s", p, n && p[n - 1] == '/' ? "" : "/");
    l_info("chdir(%s) -> %s", path, cwd);
    return 0;
}
// POSIX form: no trailing slash (CFileStream::Open appends "/name" to it).
static char *d_getcwd(char *buf, size_t size) {
    snprintf(buf, size, "%s", cwd);
    size_t n = strlen(buf);
    if (n > 1 && buf[n - 1] == '/' && buf[n - 2] != ':')
        buf[n - 1] = 0;
    return buf;
}

// Darwin struct utsname: 5 x char[256].
static int d_uname(char (*u)[256]) {
    static const char *f[5] = {"Darwin", "vita", "9.0.0", "PS Vita", "iPhone2,1"};
    for (int i = 0; i < 5; i++)
        snprintf(u[i], 256, "%s", f[i]);
    return 0;
}
static int d_gethostname(char *name, size_t len) {
    snprintf(name, len, "vita");
    return 0;
}

// Darwin struct tm has tm_gmtoff/tm_zone after the 9 ints newlib shares: they are zeroed.
static struct tm *d_localtime_r(const time_t *t, uint32_t *out) {
    localtime_r(t, (struct tm *)out);
    out[9] = out[10] = 0;
    return (struct tm *)out;
}

static int d_nanosleep(const struct timespec *req, struct timespec *rem) {
    sceKernelDelayThread(req->tv_sec * 1000000 + req->tv_nsec / 1000);
    return 0;
}
static int d_sched_yield(void) {
    sceKernelDelayThread(0);
    return 0;
}
static int d_ok(void) { return 0; }
static void *d_null(void) { return NULL; }
static char *d_inet_ntoa(uint32_t in) { return "0.0.0.0"; }
static int d___tolower(int c) { return tolower(c); }

static int d_printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, sizeof(buf), fmt, &w);
    va_end(ap);
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '\n')
        buf[n - 1] = 0;
    l_info("printf: %s", buf);
    return r;
}
static int d_snprintf(char *buf, size_t len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, len, fmt, &w);
    va_end(ap);
    return r;
}
// Wide printf through the Darwin formatter: narrow the (ASCII) format, format, widen the result.
static int d_swprintf(wchar_t *buf, size_t len, const wchar_t *wfmt, ...) {
    char fmt[512], out[1024];
    size_t i;
    for (i = 0; wfmt[i] && i < sizeof(fmt) - 1; i++)
        fmt[i] = (char)wfmt[i];
    fmt[i] = 0;
    va_list ap;
    va_start(ap, wfmt);
    words w = {&ap, NULL};
    darwin_vformat(out, sizeof(out), fmt, &w);
    va_end(ap);
    for (i = 0; out[i] && i + 1 < len; i++)
        buf[i] = (unsigned char)out[i];
    if (len)
        buf[i] = 0;
    return i;
}

// ---------------------------------------------------------------- networking (offline stub)

static int d_net_fail(void) {
    errno = ENETDOWN;
    return -1;
}
static void *d_gethostbyname(const char *n) { return NULL; }
static int d_select(int n, void *r, void *w, void *e, void *t) { return 0; }

// ---------------------------------------------------------------- pthreads

// Darwin: pthread_mutex_t = {long sig; char opaque[40]}. Vita mutex stored in opaque.
// Darwin PTHREAD_MUTEX_INITIALIZER signature: 0x32AAABA7 (lazy initialized on first use).
#define DARWIN_MUTEX_SIG_INIT 0x32AAABA7
#define ML_MUTEX_SIG 0x4D4C5458
#define ML_MUTEXATTR_SIG 0x4D4C5441
typedef struct { uint32_t sig; pthread_mutex_t m; } d_mutex;
typedef struct { uint32_t sig; int type; } d_mutexattr;

static int d_pthread_mutexattr_init(d_mutexattr *a) {
    a->sig = ML_MUTEXATTR_SIG;
    a->type = 0;
    return 0;
}
static int d_pthread_mutexattr_settype(d_mutexattr *a, int type) {
    a->type = type; // Darwin: 0 normal, 1 errorcheck, 2 recursive
    return 0;
}
static int d_pthread_mutex_init(d_mutex *m, const d_mutexattr *a) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (a && a->sig == ML_MUTEXATTR_SIG)
        pthread_mutexattr_settype(&attr, a->type == 2 ? PTHREAD_MUTEX_RECURSIVE
                                         : a->type == 1 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    int r = pthread_mutex_init(&m->m, &attr);
    pthread_mutexattr_destroy(&attr);
    m->sig = ML_MUTEX_SIG;
    return r;
}
static void mutex_lazy(d_mutex *m) {
    if (m->sig != ML_MUTEX_SIG) {
        if (m->sig != DARWIN_MUTEX_SIG_INIT)
            l_error("uninitialized mutex (sig 0x%08X) at %p", (unsigned)m->sig, m);
        d_pthread_mutex_init(m, NULL);
    }
}
static int d_pthread_mutex_lock(d_mutex *m) {
    mutex_lazy(m);
    return pthread_mutex_lock(&m->m);
}
static int d_pthread_mutex_unlock(d_mutex *m) {
    mutex_lazy(m);
    return pthread_mutex_unlock(&m->m);
}
static int d_pthread_mutex_destroy(d_mutex *m) {
    if (m->sig != ML_MUTEX_SIG)
        return 0;
    m->sig = 0;
    return pthread_mutex_destroy(&m->m);
}

typedef struct { void *fn, *arg; } guest_thread;
static void *thread_tramp(void *p) {
    guest_thread t = *(guest_thread *)p;
    free(p);
    return (void *)call_guest(t.fn, (uint32_t)t.arg, 0, 0, 0);
}
static int d_pthread_create(pthread_t *thr, const void *darwin_attr, void *fn, void *arg) {
    guest_thread *t = malloc(sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024); // iOS default 512 KB secondary thread stack
    int r = pthread_create(thr, &attr, thread_tramp, t);
    pthread_attr_destroy(&attr);
    l_debug("pthread_create(0x%08X) -> %d", (unsigned)fn - MACHO_SLIDE, r);
    return r;
}
static int d_pthread_detach(pthread_t t) {
    if (!t)
        return 0;
    // If handle is a Sony Vita SceUID (0x4001xxxx)
    if (((uintptr_t)t & 0xFFFF0000) == 0x40010000) {
        l_info("pthread_detach(SceUID 0x%08X) -> ok", (unsigned)(uintptr_t)t);
        return 0;
    }
    // If handle is a pointer to pte_thread_t in user memory
    uintptr_t addr = (uintptr_t)t;
    if (addr >= 0x81000000 && addr < 0x90000000 && (addr & 3) == 0) {
        uint32_t first = *(uint32_t *)addr;
        if ((first & 0xFFFF0000) == 0x40010000) {
            return pthread_detach(t);
        }
    }
    l_info("pthread_detach(%p) safely ignored", (void *)t);
    return 0;
}

static pthread_t d_pthread_self(void) {
    pthread_t p = pthread_self();
    if (p)
        return p;
    return (pthread_t)sceKernelGetThreadId();
}

static int d_pthread_setschedparam(pthread_t t, int policy, const void *param) { return 0; }

// Named semaphores (FMOD): Darwin sem_t* -> Vita SceUID.
typedef struct { SceUID id; } d_sem;
static d_sem *d_sem_open(const char *name, int oflag, ...) {
    va_list ap;
    va_start(ap, oflag);
    va_arg(ap, int); // mode
    unsigned value = va_arg(ap, unsigned);
    va_end(ap);
    d_sem *s = malloc(sizeof(*s));
    s->id = sceKernelCreateSema("ml_sem", 0, value, 0x7FFFFFFF, NULL);
    if (s->id < 0) {
        free(s);
        return (d_sem *)-1; // SEM_FAILED
    }
    return s;
}
static int d_sem_close(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    sceKernelDeleteSema(s->id);
    free(s);
    return 0;
}
static int d_sem_unlink(const char *name) { return 0; }
static int d_sem_wait(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    return sceKernelWaitSema(s->id, 1, NULL) < 0 ? -1 : 0;
}
static int d_sem_post(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    return sceKernelSignalSema(s->id, 1) < 0 ? -1 : 0;
}

// ---------------------------------------------------------------- C++ / blocks / exceptions

static int d___cxa_atexit(void *f, void *a, void *d) { return 0; } // games never terminate cleanly
static int d___cxa_guard_acquire(uint8_t *g) { return *g == 0; }
static void d___cxa_guard_release(uint8_t *g) { *g = 1; }
static void d___cxa_pure_virtual(void) { fatal_error("__cxa_pure_virtual"); }
static void d_sjlj_noop(void *ctx) {}
static void d_sjlj_resume(void *ctx) { fatal_error("C++/ObjC exception thrown (_Unwind_SjLj_Resume)"); }

static uint8_t d_NSConcreteStackBlock[32];
static void d_Block_object_assign(void **dst, void *src, int flags) { *dst = src; }
static void d_Block_object_dispose(void *obj, int flags) {}

static uint8_t d_dispatch_main_q[64];
static void *d_dispatch_get_global_queue(long prio, unsigned long flags) { return d_dispatch_main_q; }
static void d_dispatch_async(void *q, void *block) { l_debug("dispatch_async ignored"); }

// ---------------------------------------------------------------- libgcc softfp (Apple *vfp)

SOFTFP static float f_add(float a, float b) { return a + b; }
SOFTFP static float f_sub(float a, float b) { return a - b; }
SOFTFP static float f_mul(float a, float b) { return a * b; }
SOFTFP static float f_div(float a, float b) { return a / b; }
SOFTFP static double d_add(double a, double b) { return a + b; }
SOFTFP static double d_sub(double a, double b) { return a - b; }
SOFTFP static double d_mul(double a, double b) { return a * b; }
// compiler-rt *vfp comparisons: return 1 if condition is true (unlike libgcc -1/0/1).
SOFTFP static int f_eq(float a, float b) { return a == b; }
SOFTFP static int f_ne(float a, float b) { return !(a == b); }
SOFTFP static int f_gt(float a, float b) { return a > b; }
SOFTFP static int f_ge(float a, float b) { return a >= b; }
SOFTFP static int f_lt(float a, float b) { return a < b; }
SOFTFP static int f_le(float a, float b) { return a <= b; }
SOFTFP static int dd_gt(double a, double b) { return a > b; }
SOFTFP static int dd_ge(double a, double b) { return a >= b; }
SOFTFP static int dd_lt(double a, double b) { return a < b; }
SOFTFP static double f_ext(float a) { return a; }
SOFTFP static float d_trunc(double a) { return (float)a; }
SOFTFP static int f_toi(float a) { return (int)a; }
SOFTFP static int d_toi(double a) { return (int)a; }
SOFTFP static long long f_toll(float a) { return (long long)a; }
SOFTFP static long long d_toll(double a) { return (long long)a; }
SOFTFP static unsigned long long f_toull(float a) { return (unsigned long long)a; }
SOFTFP static float i_tof(int a) { return (float)a; }
SOFTFP static float u_tof(unsigned a) { return (float)a; }
SOFTFP static double i_tod(int a) { return (double)a; }
SOFTFP static double ll_tod(long long a) { return (double)a; }
SOFTFP static float ll_tof(long long a) { return (float)a; }

static int i_div(int a, int b) { return a / b; }
static int i_mod(int a, int b) { return a % b; }
static unsigned u_div(unsigned a, unsigned b) { return a / b; }
static unsigned u_mod(unsigned a, unsigned b) { return a % b; }
static long long ll_div(long long a, long long b) { return a / b; }
static long long ll_mod(long long a, long long b) { return a % b; }
static long long ll_mul(long long a, long long b) { return a * b; }
static unsigned long long ull_div(unsigned long long a, unsigned long long b) { return a / b; }
static unsigned long long ull_mod(unsigned long long a, unsigned long long b) { return a % b; }
static int ll_cmp(long long a, long long b) { return a < b ? 0 : a == b ? 1 : 2; }

SOFTFP static float m_acosf(float x) { return acosf(x); }
SOFTFP static float m_cosf(float x) { return cosf(x); }
SOFTFP static float m_sinf(float x) { return sinf(x); }
SOFTFP static float m_tanf(float x) { return tanf(x); }
SOFTFP static float m_expf(float x) { return expf(x); }
SOFTFP static float m_logf(float x) { return logf(x); }
SOFTFP static float m_log10f(float x) { return log10f(x); }
SOFTFP static float m_floorf(float x) { return floorf(x); }
SOFTFP static long m_lrintf(float x) { return lrintf(x); }
SOFTFP static double m_cos(double x) { return cos(x); }
SOFTFP static double m_sin(double x) { return sin(x); }
SOFTFP static double m_floor(double x) { return floor(x); }
SOFTFP static double m_log10(double x) { return log10(x); }
SOFTFP static double m_atan2(double y, double x) { return atan2(y, x); }
SOFTFP static double m_fmod(double x, double y) { return fmod(x, y); }
SOFTFP static double m_pow(double x, double y) { return pow(x, y); }
SOFTFP static double m_ldexp(double x, int e) { return ldexp(x, e); }

// ---------------------------------------------------------------- import table

// name without Mach-O prepended "_": F(__divsi3, ...) registers "___divsi3".
#define F(name, fn) {"_" #name, (uintptr_t)(fn)}
#define D(name) {"_" #name, (uintptr_t)(name)} // identical signature in newlib
#define V(name, ptr) {"_" #name, (uintptr_t)(ptr), 1} // data symbol, not function

const import_entry darwin_imports[] = {
    // memory / strings
    D(malloc), D(calloc), D(realloc), D(free), D(memcpy), D(memmove), D(memset), D(memcmp),
    D(memchr), D(bzero), D(strlen), D(strcmp), D(strcpy), D(strncpy), D(strstr), D(strtok),
    D(atoi), D(atol), D(atof), D(strtol), D(strcat), D(strchr), D(strrchr), D(strcspn), D(strncmp),
    D(strcasecmp), D(strncasecmp), D(strlcpy), F(__tolower, d___tolower), D(wcscmp), D(wcscpy),
    D(wcslen), D(swscanf), F(swprintf, d_swprintf), F(printf, d_printf), F(snprintf, d_snprintf),
    D(putchar), D(fgets),
    D(rand), D(srand), F(sprintf, d_sprintf), F(vsprintf, d_vsprintf),
    F(vsnprintf, d_vsnprintf), D(sscanf), D(puts),
    F(qsort, d_qsort), F(_ZdlPv, free),
    F(__memcpy_chk, d___memcpy_chk), F(__strcpy_chk, d___strcpy_chk),
    F(__sprintf_chk, d___sprintf_chk), F(__assert_rtn, d___assert_rtn), F(__error, d___error),
    F(NSLog, d_NSLog),
    // stdio
    F(fopen, d_fopen), D(fclose), D(fread), D(fwrite), D(fseek), D(ftell), D(feof), D(ferror),
    D(fflush), D(fputs), F(mkdir, d_mkdir), D(close), F(stat, d_stat), F(opendir, d_opendir),
    F(readdir, d_readdir), F(closedir, d_closedir), F_RAW("_realpath$DARWIN_EXTSN", d_realpath),
    F(chdir, d_chdir), F(getcwd, d_getcwd), V(__stderrp, &d_stderrp),
    V(__stdinp, &d_stdinp),
    // time / system
    D(gettimeofday), F(usleep, d_usleep), F(mach_absolute_time, d_mach_absolute_time),
    F(mach_timebase_info, d_mach_timebase_info), F(sysctlbyname, d_sysctlbyname), F(exit, d_exit),
    D(time), D(mktime), D(strftime), F(localtime_r, d_localtime_r), F(nanosleep, d_nanosleep),
    F(sched_yield, d_sched_yield), F(uname, d_uname), F(gethostname, d_gethostname),
    F(sigaction, d_ok),
    // network: offline stubs
    F(socket, d_net_fail), F(connect, d_net_fail), F(bind, d_net_fail), F(listen, d_net_fail),
    F(accept, d_net_fail), F(send, d_net_fail), F(recv, d_net_fail), F(setsockopt, d_net_fail),
    F(fcntl, d_net_fail), F(inet_addr, d_net_fail), F(gethostbyname, d_gethostbyname),
    F(select, d_select), F(getsockname, d_net_fail), F(getsockopt, d_net_fail),
    F(recvfrom, d_net_fail), F(sendto, d_net_fail), F(ioctl, d_net_fail),
    F(getprotobyname, d_null), F(inet_ntoa, d_inet_ntoa),
    // pthreads / semaphores
    F(pthread_create, d_pthread_create), F(pthread_detach, d_pthread_detach), F(pthread_self, d_pthread_self),
    F(pthread_setschedparam, d_pthread_setschedparam), F(pthread_cancel, d_ok),
    F(pthread_setcanceltype, d_ok), F(pthread_testcancel, d_ok), F(pthread_mutex_init, d_pthread_mutex_init),
    F(pthread_mutex_lock, d_pthread_mutex_lock), F(pthread_mutex_unlock, d_pthread_mutex_unlock),
    F(pthread_mutex_destroy, d_pthread_mutex_destroy), F(pthread_mutexattr_init, d_pthread_mutexattr_init),
    F(pthread_mutexattr_settype, d_pthread_mutexattr_settype), F(sem_open, d_sem_open),
    F(sem_close, d_sem_close), F(sem_unlink, d_sem_unlink), F(sem_wait, d_sem_wait),
    F(sem_post, d_sem_post),
    // C++ / blocks / exceptions / GCD
    F(__cxa_atexit, d___cxa_atexit), F(__cxa_guard_acquire, d___cxa_guard_acquire),
    F(__cxa_guard_release, d___cxa_guard_release), F(__cxa_pure_virtual, d___cxa_pure_virtual),
    F(_Unwind_SjLj_Register, d_sjlj_noop), F(_Unwind_SjLj_Unregister, d_sjlj_noop),
    F(_Unwind_SjLj_Resume, d_sjlj_resume), V(_NSConcreteStackBlock, d_NSConcreteStackBlock),
    F(_Block_object_assign, d_Block_object_assign), F(_Block_object_dispose, d_Block_object_dispose),
    V(_dispatch_main_q, d_dispatch_main_q), F(dispatch_get_global_queue, d_dispatch_get_global_queue),
    F(dispatch_async, d_dispatch_async),
    // softfp helpers
    F(__addsf3vfp, f_add), F(__subsf3vfp, f_sub), F(__mulsf3vfp, f_mul), F(__divsf3vfp, f_div),
    F(__adddf3vfp, d_add), F(__subdf3vfp, d_sub), F(__muldf3vfp, d_mul),
    F(__eqsf2vfp, f_eq), F(__nesf2vfp, f_ne), F(__gtsf2vfp, f_gt), F(__gesf2vfp, f_ge),
    F(__ltsf2vfp, f_lt), F(__lesf2vfp, f_le), F(__gtdf2vfp, dd_gt), F(__gedf2vfp, dd_ge),
    F(__ltdf2vfp, dd_lt), F(__extendsfdf2vfp, f_ext), F(__truncdfsf2vfp, d_trunc),
    F(__fixsfsivfp, f_toi), F(__fixdfsivfp, d_toi), F(__fixsfdi, f_toll), F(__fixdfdi, d_toll),
    F(__fixunssfdi, f_toull), F(__floatsisfvfp, i_tof), F(__floatunssisfvfp, u_tof),
    F(__floatsidfvfp, i_tod), F(__floatdidf, ll_tod), F(__floatdisf, ll_tof),
    F(__divsi3, i_div), F(__modsi3, i_mod), F(__udivsi3, u_div), F(__umodsi3, u_mod),
    F(__divdi3, ll_div), F(__moddi3, ll_mod), F(__muldi3, ll_mul), F(__udivdi3, ull_div),
    F(__umoddi3, ull_mod), F(__cmpdi2, ll_cmp),
    // math softfp
    F(acosf, m_acosf), F(cosf, m_cosf), F(sinf, m_sinf), F(tanf, m_tanf), F(expf, m_expf),
    F(logf, m_logf), F(log10f, m_log10f), F(floorf, m_floorf), F(lrintf, m_lrintf), F(cos, m_cos),
    F(sin, m_sin), F(floor, m_floor), F(log10, m_log10), F(atan2, m_atan2), F(fmod, m_fmod),
    F(pow, m_pow), F(ldexp, m_ldexp), D(acos), D(asin), D(atan), D(tan), D(asinf), D(atanf),
    D(atan2f), D(ceilf), D(fmodf),
};
const int darwin_imports_count = sizeof(darwin_imports) / sizeof(darwin_imports[0]);

void darwin_init(void) {
    d_stderrp = stderr;
    d_stdinp = stdin;
}
