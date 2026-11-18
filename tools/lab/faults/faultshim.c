/* LD_PRELOAD file-system fault shim for the put.gd fault-injection harness (tools/lab/action_fuzz.py faults).
 *
 * It wraps the libc calls a Qt/SQLite program writes files with and makes them fail on a rule, so a run sees the disk
 * saying no: ENOSPC, EIO, EACCES, EROFS, EMFILE, a short write, a failing fsync or rename, a slow disk.
 *
 *   PUTGD_FAULT_SPEC=/path/spec   the rule file; it is re-read when its mtime changes (at most every 50 ms), so a
 *                                 harness changes faults while the program runs. A missing file means no faults.
 *
 * Spec lines (blank lines and # comments are ignored):
 *   seed N                          seeds the generator (xorshift, mixed with the process id)
 *   log PATH                        append one line per injected fault: seconds pid comm op errno-or-action path
 *   rule OPS PATHS PROB ACTION [comm=NAME] [skip=N] [count=N]
 *      OPS    comma list of: open write read fsync rename truncate fallocate unlink mkdir copy close, or *
 *      PATHS  '|' separated substrings of the path the call touches (rename: either side), or *
 *      PROB   0..1
 *      ACTION an errno name (ENOSPC EIO EACCES EROFS EMFILE ENOMEM EINTR EDQUOT ENOENT EXDEV EBUSY ...),
 *             short (a write writes about half of what it was given), delay:MS (sleep, then run normally)
 *      comm=  only processes whose name (comm) starts with it; skip=N lets the first N matches through;
 *             count=N stops after N injected faults (per process)
 *
 * Build: gcc -O2 -shared -fPIC -o faultshim.so faultshim.c -ldl -lpthread
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

enum { OP_OPEN, OP_WRITE, OP_READ, OP_FSYNC, OP_RENAME, OP_TRUNCATE, OP_FALLOCATE, OP_UNLINK, OP_MKDIR, OP_COPY, OP_CLOSE, OP_COUNT };
static const char *const OP_NAMES[OP_COUNT] = {"open", "write", "read", "fsync", "rename", "truncate", "fallocate", "unlink", "mkdir", "copy", "close"};

#define MAX_RULES 64
#define MAX_PATHS 6
enum { ACT_ERRNO = 1, ACT_SHORT = 2, ACT_DELAY = 3 };

struct rule {
    unsigned ops;               /* bit mask */
    char paths[MAX_PATHS][96];
    int npaths;                 /* 0 = any */
    double prob;
    int action, err, delay_ms;
    char comm[32];
    long skip, count, seen, fired;
    char errname[16];
};

static struct rule rules[MAX_RULES];
static int nrules;
static char spec_path[512], log_path[512];
static struct timespec spec_mtime;
static long long last_check_ns;
static uint64_t rng_state;
static uint64_t rng_seed;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char my_comm[32];
static int logfd = -1;
static __thread int busy;

static const struct { const char *name; int value; } ERRNOS[] = {
    {"ENOSPC", ENOSPC}, {"EIO", EIO}, {"EACCES", EACCES}, {"EROFS", EROFS}, {"EMFILE", EMFILE}, {"ENFILE", ENFILE}, {"ENOMEM", ENOMEM},
    {"EINTR", EINTR}, {"EDQUOT", EDQUOT}, {"ENOENT", ENOENT}, {"EXDEV", EXDEV}, {"EBUSY", EBUSY}, {"EPERM", EPERM}, {"EAGAIN", EAGAIN},
    {"EFBIG", EFBIG}, {"ENAMETOOLONG", ENAMETOOLONG}, {"ELOOP", ELOOP}, {"EEXIST", EEXIST}, {"ENOTEMPTY", ENOTEMPTY}, {"EISDIR", EISDIR}, {"EBADF", EBADF},
};

static long long now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static uint64_t next_random(void) {
    uint64_t x = rng_state ? rng_state : 0x9E3779B97F4A7C15ULL;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return x;
}

static void reseed(uint64_t seed) {
    rng_seed = seed;
    rng_state = (seed * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)getpid() * 0xBF58476D1CE4E5B9ULL) ^ 0x1234567ULL;
    for (int i = 0; i < 8; i++) next_random();
}

static void load_comm(void) {
    if (my_comm[0]) return;
    int fd = (int)syscall(SYS_open, "/proc/self/comm", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = syscall(SYS_read, fd, my_comm, sizeof my_comm - 1);
        syscall(SYS_close, fd);
        if (n > 0) { my_comm[n] = 0; char *nl = strchr(my_comm, '\n'); if (nl) *nl = 0; }
    }
    if (!my_comm[0]) strcpy(my_comm, "?");
}

static void parse_line(char *line) {
    char *save = NULL;
    char *tok = strtok_r(line, " \t\r\n", &save);
    if (!tok || tok[0] == '#') return;
    if (!strcmp(tok, "seed")) {
        char *v = strtok_r(NULL, " \t\r\n", &save);
        if (v) { uint64_t s = strtoull(v, NULL, 10); if (s != rng_seed || !rng_state) reseed(s); }
        return;
    }
    if (!strcmp(tok, "log")) {
        char *v = strtok_r(NULL, " \t\r\n", &save);
        if (v && strcmp(v, log_path)) {
            strncpy(log_path, v, sizeof log_path - 1);
            if (logfd >= 0) syscall(SYS_close, logfd);
            logfd = (int)syscall(SYS_open, log_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
        }
        return;
    }
    if (strcmp(tok, "rule") || nrules >= MAX_RULES) return;
    struct rule *r = &rules[nrules];
    memset(r, 0, sizeof *r);
    char *ops = strtok_r(NULL, " \t\r\n", &save), *paths = strtok_r(NULL, " \t\r\n", &save), *prob = strtok_r(NULL, " \t\r\n", &save), *action = strtok_r(NULL, " \t\r\n", &save);
    if (!ops || !paths || !prob || !action) return;
    if (!strcmp(ops, "*")) r->ops = (1u << OP_COUNT) - 1;
    else {
        char *s2 = NULL;
        for (char *o = strtok_r(ops, ",", &s2); o; o = strtok_r(NULL, ",", &s2))
            for (int i = 0; i < OP_COUNT; i++) if (!strcmp(o, OP_NAMES[i])) r->ops |= 1u << i;
    }
    if (strcmp(paths, "*")) {
        char *s2 = NULL;
        for (char *p = strtok_r(paths, "|", &s2); p && r->npaths < MAX_PATHS; p = strtok_r(NULL, "|", &s2)) strncpy(r->paths[r->npaths++], p, 95);
    }
    r->prob = atof(prob);
    if (!strcmp(action, "short")) r->action = ACT_SHORT;
    else if (!strncmp(action, "delay:", 6)) { r->action = ACT_DELAY; r->delay_ms = atoi(action + 6); }
    else {
        r->action = ACT_ERRNO;
        r->err = EIO;
        for (unsigned i = 0; i < sizeof ERRNOS / sizeof *ERRNOS; i++) if (!strcmp(action, ERRNOS[i].name)) r->err = ERRNOS[i].value;
        strncpy(r->errname, action, sizeof r->errname - 1);
    }
    r->count = -1;
    char *extra;
    while ((extra = strtok_r(NULL, " \t\r\n", &save))) {
        if (!strncmp(extra, "comm=", 5)) strncpy(r->comm, extra + 5, sizeof r->comm - 1);
        else if (!strncmp(extra, "skip=", 5)) r->skip = atol(extra + 5);
        else if (!strncmp(extra, "count=", 6)) r->count = atol(extra + 6);
    }
    nrules++;
}

static void reload_if_changed(void) {
    if (!spec_path[0]) {
        const char *p = getenv("PUTGD_FAULT_SPEC");
        if (!p) return;
        strncpy(spec_path, p, sizeof spec_path - 1);
    }
    long long now = now_ns();
    if (now - last_check_ns < 50LL * 1000000LL) return;
    last_check_ns = now;
    struct stat st;
    if (syscall(SYS_stat, spec_path, &st) != 0) {
        if (nrules) { nrules = 0; }
        spec_mtime.tv_sec = 0; spec_mtime.tv_nsec = 0;
        return;
    }
    if (st.st_mtim.tv_sec == spec_mtime.tv_sec && st.st_mtim.tv_nsec == spec_mtime.tv_nsec) return;
    spec_mtime = st.st_mtim;
    int fd = (int)syscall(SYS_open, spec_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    static char buf[16384];
    ssize_t n = syscall(SYS_read, fd, buf, sizeof buf - 1);
    syscall(SYS_close, fd);
    if (n < 0) return;
    buf[n] = 0;
    nrules = 0;
    load_comm();
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char copy[512];
        strncpy(copy, line, sizeof copy - 1); copy[sizeof copy - 1] = 0;
        parse_line(copy);
    }
}

static void log_fault(const char *op, const char *what, const char *path) {
    if (logfd < 0) return;
    char line[700];
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    int n = snprintf(line, sizeof line, "%ld.%03ld %d %s %s %s %s\n", (long)t.tv_sec, t.tv_nsec / 1000000, (int)getpid(), my_comm, op, what, path ? path : "-");
    if (n > 0) syscall(SYS_write, logfd, line, (size_t)n);
}

static int path_matches(struct rule *r, const char *a, const char *b) {
    if (!r->npaths) return 1;
    for (int i = 0; i < r->npaths; i++) {
        if (a && strstr(a, r->paths[i])) return 1;
        if (b && strstr(b, r->paths[i])) return 1;
    }
    return 0;
}

/* Returns 0 to proceed, or sets *err (an errno to fail with) / *short_write. */
static int decide(int op, const char *path_a, const char *path_b, int *err, int *short_write) {
    if (busy) return 0;
    busy = 1;
    int result = 0;
    pthread_mutex_lock(&lock);
    reload_if_changed();
    int delay = 0;
    for (int i = 0; i < nrules && !result; i++) {
        struct rule *r = &rules[i];
        if (!(r->ops & (1u << op))) continue;
        if (r->comm[0]) { load_comm(); if (strncmp(my_comm, r->comm, strlen(r->comm))) continue; }
        if (!path_matches(r, path_a, path_b)) continue;
        if (r->count >= 0 && r->fired >= r->count) continue;
        if (r->seen++ < r->skip) continue;
        if ((double)(next_random() >> 11) / 9007199254740992.0 >= r->prob) continue;
        r->fired++;
        load_comm();
        if (r->action == ACT_ERRNO) { *err = r->err; result = 1; log_fault(OP_NAMES[op], r->errname, path_a ? path_a : path_b); }
        else if (r->action == ACT_SHORT) { if (op == OP_WRITE) { *short_write = 1; result = 2; log_fault(OP_NAMES[op], "short", path_a); } }
        else if (r->action == ACT_DELAY) { delay += r->delay_ms; log_fault(OP_NAMES[op], "delay", path_a ? path_a : path_b); }
    }
    pthread_mutex_unlock(&lock);
    if (delay > 0) { struct timespec ts = {delay / 1000, (delay % 1000) * 1000000L}; nanosleep(&ts, NULL); }
    busy = 0;
    return result;
}

static int active(void) {
    /* Cheap pre-check so an unconfigured process pays almost nothing. */
    if (!spec_path[0] && !getenv("PUTGD_FAULT_SPEC")) return 0;
    return 1;
}

/* The path behind a descriptor. Standard streams and the shim's own log are never faulted. */
static int fd_path(int fd, char *buf, size_t size) {
    if (fd < 3 || fd == logfd) return 0;
    char link[48];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = syscall(SYS_readlink, link, buf, size - 1);
    if (n <= 0) return 0;
    buf[n] = 0;
    return 1;
}

#define REAL(name) static __typeof__(&name) real_##name; if (!real_##name) real_##name = (__typeof__(&name))dlsym(RTLD_NEXT, #name)

/* ---- open family */
#define OPEN_BODY(real_call, path) \
    if (active()) { int err = 0, sh = 0; if (decide(OP_OPEN, path, NULL, &err, &sh) == 1) { errno = err; return -1; } } \
    return real_call

int open(const char *path, int flags, ...) {
    REAL(open);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    OPEN_BODY(real_open(path, flags, mode), path);
}
int open64(const char *path, int flags, ...) {
    REAL(open64);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    OPEN_BODY(real_open64(path, flags, mode), path);
}
int openat(int dirfd, const char *path, int flags, ...) {
    REAL(openat);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    OPEN_BODY(real_openat(dirfd, path, flags, mode), path);
}
int openat64(int dirfd, const char *path, int flags, ...) {
    REAL(openat64);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    OPEN_BODY(real_openat64(dirfd, path, flags, mode), path);
}
int creat(const char *path, mode_t mode) { REAL(creat); OPEN_BODY(real_creat(path, mode), path); }
int creat64(const char *path, mode_t mode) { REAL(creat64); OPEN_BODY(real_creat64(path, mode), path); }

/* ---- write family */
#define FD_DECIDE(op, fd, retval_on_err) \
    char fdp[512]; \
    if (active() && fd_path(fd, fdp, sizeof fdp)) { \
        int err = 0, sh = 0; int d = decide(op, fdp, NULL, &err, &sh); \
        if (d == 1) { errno = err; return retval_on_err; } \
        short_write = (d == 2); \
    }

ssize_t write(int fd, const void *buf, size_t n) {
    REAL(write);
    int short_write = 0;
    FD_DECIDE(OP_WRITE, fd, -1)
    if (short_write && n > 1) n = n / 2;
    return real_write(fd, buf, n);
}
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
    REAL(pwrite);
    int short_write = 0;
    FD_DECIDE(OP_WRITE, fd, -1)
    if (short_write && n > 1) n = n / 2;
    return real_pwrite(fd, buf, n, off);
}
ssize_t pwrite64(int fd, const void *buf, size_t n, off64_t off) {
    REAL(pwrite64);
    int short_write = 0;
    FD_DECIDE(OP_WRITE, fd, -1)
    if (short_write && n > 1) n = n / 2;
    return real_pwrite64(fd, buf, n, off);
}
ssize_t writev(int fd, const struct iovec *iov, int count) {
    REAL(writev);
    int short_write = 0;
    FD_DECIDE(OP_WRITE, fd, -1)
    if (short_write && count > 1) count = count / 2;
    else if (short_write && count == 1) { struct iovec one = {iov[0].iov_base, iov[0].iov_len > 1 ? iov[0].iov_len / 2 : iov[0].iov_len}; return real_writev(fd, &one, 1); }
    return real_writev(fd, iov, count);
}
ssize_t read(int fd, void *buf, size_t n) {
    REAL(read);
    int short_write = 0;
    FD_DECIDE(OP_READ, fd, -1)
    (void)short_write;
    return real_read(fd, buf, n);
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    REAL(pread);
    int short_write = 0;
    FD_DECIDE(OP_READ, fd, -1)
    (void)short_write;
    return real_pread(fd, buf, n, off);
}
ssize_t pread64(int fd, void *buf, size_t n, off64_t off) {
    REAL(pread64);
    int short_write = 0;
    FD_DECIDE(OP_READ, fd, -1)
    (void)short_write;
    return real_pread64(fd, buf, n, off);
}

int fsync(int fd) { REAL(fsync); int short_write = 0; FD_DECIDE(OP_FSYNC, fd, -1) (void)short_write; return real_fsync(fd); }
int fdatasync(int fd) { REAL(fdatasync); int short_write = 0; FD_DECIDE(OP_FSYNC, fd, -1) (void)short_write; return real_fdatasync(fd); }
int ftruncate(int fd, off_t len) { REAL(ftruncate); int short_write = 0; FD_DECIDE(OP_TRUNCATE, fd, -1) (void)short_write; return real_ftruncate(fd, len); }
int ftruncate64(int fd, off64_t len) { REAL(ftruncate64); int short_write = 0; FD_DECIDE(OP_TRUNCATE, fd, -1) (void)short_write; return real_ftruncate64(fd, len); }
int fallocate(int fd, int mode, off_t off, off_t len) {
    REAL(fallocate);
    int short_write = 0;
    FD_DECIDE(OP_FALLOCATE, fd, -1)
    (void)short_write;
    return real_fallocate(fd, mode, off, len);
}
int fallocate64(int fd, int mode, off64_t off, off64_t len) {
    REAL(fallocate64);
    int short_write = 0;
    FD_DECIDE(OP_FALLOCATE, fd, -1)
    (void)short_write;
    return real_fallocate64(fd, mode, off, len);
}
int posix_fallocate(int fd, off_t off, off_t len) {
    REAL(posix_fallocate);
    char fdp[512];
    if (active() && fd_path(fd, fdp, sizeof fdp)) { int err = 0, sh = 0; if (decide(OP_FALLOCATE, fdp, NULL, &err, &sh) == 1) return err; }
    return real_posix_fallocate(fd, off, len);
}
int posix_fallocate64(int fd, off64_t off, off64_t len) {
    REAL(posix_fallocate64);
    char fdp[512];
    if (active() && fd_path(fd, fdp, sizeof fdp)) { int err = 0, sh = 0; if (decide(OP_FALLOCATE, fdp, NULL, &err, &sh) == 1) return err; }
    return real_posix_fallocate64(fd, off, len);
}
ssize_t copy_file_range(int in, off64_t *io, int out, off64_t *oo, size_t len, unsigned flags) {
    REAL(copy_file_range);
    int short_write = 0;
    FD_DECIDE(OP_COPY, out, -1)
    (void)short_write;
    return real_copy_file_range(in, io, out, oo, len, flags);
}
ssize_t sendfile(int out, int in, off_t *off, size_t count) {
    REAL(sendfile);
    int short_write = 0;
    FD_DECIDE(OP_COPY, out, -1)
    (void)short_write;
    return real_sendfile(out, in, off, count);
}
ssize_t sendfile64(int out, int in, off64_t *off, size_t count) {
    REAL(sendfile64);
    int short_write = 0;
    FD_DECIDE(OP_COPY, out, -1)
    (void)short_write;
    return real_sendfile64(out, in, off, count);
}
int close(int fd) {
    REAL(close);
    char fdp[512];
    if (active() && fd > 2 && fd != logfd && !busy) {
        if (fd_path(fd, fdp, sizeof fdp)) {
            int err = 0, sh = 0;
            if (decide(OP_CLOSE, fdp, NULL, &err, &sh) == 1) { int r = real_close(fd); (void)r; errno = err; return -1; }
        }
    }
    return real_close(fd);
}

/* ---- path operations */
#define PATH_FAIL(op, a, b) if (active()) { int err = 0, sh = 0; if (decide(op, a, b, &err, &sh) == 1) { errno = err; return -1; } }
int rename(const char *a, const char *b) { REAL(rename); PATH_FAIL(OP_RENAME, a, b) return real_rename(a, b); }
int renameat(int fa, const char *a, int fb, const char *b) { REAL(renameat); PATH_FAIL(OP_RENAME, a, b) return real_renameat(fa, a, fb, b); }
int renameat2(int fa, const char *a, int fb, const char *b, unsigned flags) { REAL(renameat2); PATH_FAIL(OP_RENAME, a, b) return real_renameat2(fa, a, fb, b, flags); }
int truncate(const char *path, off_t len) { REAL(truncate); PATH_FAIL(OP_TRUNCATE, path, NULL) return real_truncate(path, len); }
int truncate64(const char *path, off64_t len) { REAL(truncate64); PATH_FAIL(OP_TRUNCATE, path, NULL) return real_truncate64(path, len); }
int unlink(const char *path) { REAL(unlink); PATH_FAIL(OP_UNLINK, path, NULL) return real_unlink(path); }
int unlinkat(int dirfd, const char *path, int flags) { REAL(unlinkat); PATH_FAIL(OP_UNLINK, path, NULL) return real_unlinkat(dirfd, path, flags); }
int mkdir(const char *path, mode_t mode) { REAL(mkdir); PATH_FAIL(OP_MKDIR, path, NULL) return real_mkdir(path, mode); }
int mkdirat(int dirfd, const char *path, mode_t mode) { REAL(mkdirat); PATH_FAIL(OP_MKDIR, path, NULL) return real_mkdirat(dirfd, path, mode); }
