/* Minimal hand-written "SDK" for the freestanding fixtures: just the declarations of CIDER's libSystem subset.
   Layouts and constants are the Darwin x86_64 ones (they differ from Linux and Windows). */
#ifndef CIDER_DARWIN_H
#define CIDER_DARWIN_H

typedef unsigned long size_t;
typedef long ssize_t;
typedef long off_t;
typedef long time_t;
typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, t) __builtin_va_arg(v, t)
#define NULL ((void *)0)

/* stdio: FILE is opaque; stdin/stdout/stderr are the variables __stdinp/__stdoutp/__stderrp, as on macOS. */
typedef struct __sFILE FILE;
extern FILE *__stdinp, *__stdoutp, *__stderrp;
#define stdin __stdinp
#define stdout __stdoutp
#define stderr __stderrp
int puts(const char *);
int putchar(int);
int fputs(const char *, FILE *);
int fputc(int, FILE *);
int putc(int, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
int fflush(FILE *);
int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
int sprintf(char *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int vprintf(const char *, va_list);
int vfprintf(FILE *, const char *, va_list);
int vsprintf(char *, const char *, va_list);
int vsnprintf(char *, size_t, const char *, va_list);

/* POSIX */
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_APPEND 0x0008
#define O_CREAT 0x0200
#define O_TRUNC 0x0400
#define O_EXCL 0x0800
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFCHR 0020000
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#define ENOENT 2
#define EBADF 9
#define EEXIST 17
#define EINVAL 22
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 6

struct timespec { long tv_sec; long tv_nsec; };
struct timeval { long tv_sec; int tv_usec; };
struct stat {                     /* Darwin x86_64, 64-bit inodes: 144 bytes */
    int st_dev; unsigned short st_mode; unsigned short st_nlink; unsigned long long st_ino;
    unsigned st_uid; unsigned st_gid; int st_rdev; int __pad0;
    struct timespec st_atimespec, st_mtimespec, st_ctimespec, st_birthtimespec;
    long long st_size; long long st_blocks; int st_blksize; unsigned st_flags; unsigned st_gen; int st_lspare; long long st_qspare[2];
};
int open(const char *, int, ...);
int close(int);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
off_t lseek(int, off_t, int);
int stat(const char *, struct stat *) __asm("_stat$INODE64");
int fstat(int, struct stat *) __asm("_fstat$INODE64");
int access(const char *, int);
char *getcwd(char *, size_t);
int gettimeofday(struct timeval *, void *);
int clock_gettime(int, struct timespec *);
time_t time(time_t *);
int nanosleep(const struct timespec *, struct timespec *);
int usleep(unsigned);
int getpid(void);
int getppid(void);
void exit(int) __attribute__((noreturn));
void _exit(int) __attribute__((noreturn));
int *__error(void);
#define errno (*__error())

/* FILE streams */
#define EOF (-1)
FILE *fopen(const char *, const char *);
int fclose(FILE *);
size_t fread(void *, size_t, size_t, FILE *);
int fseek(FILE *, long, int);
long ftell(FILE *);
void rewind(FILE *);
int feof(FILE *);
int ferror(FILE *);
void clearerr(FILE *);
char *fgets(char *, int, FILE *);
int fgetc(FILE *);
int ungetc(int, FILE *);
int fileno(FILE *);

/* pthreads (Darwin layouts) */
typedef unsigned long pthread_t;
typedef struct { long sig; char opaque[56]; } pthread_mutex_t;
typedef struct { long sig; char opaque[8]; } pthread_mutexattr_t;
typedef struct { long sig; char opaque[40]; } pthread_cond_t;
typedef struct { long sig; char opaque[56]; } pthread_attr_t;
typedef struct { long sig; char opaque[8]; } pthread_once_t;
#define PTHREAD_MUTEX_INITIALIZER {0x32AAABA7, {0}}
#define PTHREAD_COND_INITIALIZER {0x3CB0B1BB, {0}}
#define PTHREAD_ONCE_INIT {0x30B1BCBA, {0}}
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_ERRORCHECK 1
#define PTHREAD_MUTEX_RECURSIVE 2
#define EDEADLK 11
#define EBUSY 16
#define EPERM 1
#define ESRCH 3
#define EAGAIN 35
int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
int pthread_detach(pthread_t);
pthread_t pthread_self(void);
int pthread_equal(pthread_t, pthread_t);
void pthread_exit(void *) __attribute__((noreturn));
int sched_yield(void);
int pthread_attr_init(pthread_attr_t *);
int pthread_attr_destroy(pthread_attr_t *);
int pthread_attr_setstacksize(pthread_attr_t *, size_t);
int pthread_mutexattr_init(pthread_mutexattr_t *);
int pthread_mutexattr_destroy(pthread_mutexattr_t *);
int pthread_mutexattr_settype(pthread_mutexattr_t *, int);
int pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int pthread_mutex_destroy(pthread_mutex_t *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_trylock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_cond_init(pthread_cond_t *, const void *);
int pthread_cond_destroy(pthread_cond_t *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_cond_signal(pthread_cond_t *);
int pthread_cond_broadcast(pthread_cond_t *);
int pthread_once(pthread_once_t *, void (*)(void));

/* callbacks */
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void *bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
int atexit(void (*)(void));

/* libc odds and ends */
size_t strlen(const char *);
int strcmp(const char *, const char *);
char *strcpy(char *, const char *);
char *strchr(const char *, int);
char *strstr(const char *, const char *);
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
int memcmp(const void *, const void *, size_t);
void *malloc(size_t);
void free(void *);
double sqrt(double);
double ldexp(double, int);
double frexp(double, int *);
double fmod(double, double);
double sin(double);
double atan2(double, double);
typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
div_t div(int, int);
ldiv_t ldiv(long, long);

#endif
