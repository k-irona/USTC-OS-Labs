#include "user.h"
#include "syscall.h"

static inline long __syscall(long num, long a0, long a1, long a2) {
    register long _a0 asm("a0") = a0;
    register long _a1 asm("a1") = a1;
    register long _a2 asm("a2") = a2;
    register long _a7 asm("a7") = num;
    asm volatile("ecall" : "+r"(_a0) : "r"(_a1), "r"(_a2), "r"(_a7) : "memory");
    return _a0;
}

int write(int fd, const void *buf, int n) {
    return (int)__syscall(SYS_write, fd, (long)buf, n);
}

int read(int fd, void *buf, int n) {
    return (int)__syscall(SYS_read, fd, (long)buf, n);
}

void yield(void) {
    (void)__syscall(SYS_yield, 0, 0, 0);
}

void exit(int status) {
    (void)__syscall(SYS_exit, status, 0, 0);
    for (;;) {
        // Should not return.
    }
}

void *sbrk(int n) {
    return (void *)__syscall(SYS_sbrk, n, 0, 0);
}

int fork(void) {
    return (int)__syscall(SYS_fork, 0, 0, 0);
}

int wait(int *status) {
    return (int)__syscall(SYS_wait, (long)status, 0, 0);
}

int exec(const char *name, char *const argv[]) {
    return (int)__syscall(SYS_exec, (long)name, (long)argv, 0);
}

int close(int fd) {
    return (int)__syscall(SYS_close, fd, 0, 0);
}

int fstat(int fd, struct stat *st) {
    return (int)__syscall(SYS_fstat, fd, (long)st, 0);
}

int chdir(const char *path) {
    return (int)__syscall(SYS_chdir, (long)path, 0, 0);
}

int dup(int fd) {
    return (int)__syscall(SYS_dup, fd, 0, 0);
}

int open(const char *name, int omode) {
    return (int)__syscall(SYS_open, (long)name, omode, 0);
}

int getpid(void) {
    return (int)__syscall(SYS_getpid, 0, 0, 0);
}

int sleep(int ticks) {
    return (int)__syscall(SYS_sleep, ticks, 0, 0);
}

int uptime(void) {
    return (int)__syscall(SYS_uptime, 0, 0, 0);
}

int kill(int pid) {
    return (int)__syscall(SYS_kill, pid, 0, 0);
}

int pipe(int fd[2]) {
    return (int)__syscall(SYS_pipe, (long)fd, 0, 0);
}

int mkdir(const char *path) {
    return (int)__syscall(SYS_mkdir, (long)path, 0, 0);
}

int unlink(const char *path) {
    return (int)__syscall(SYS_unlink, (long)path, 0, 0);
}

int link(const char *oldpath, const char *newpath) {
    return (int)__syscall(SYS_link, (long)oldpath, (long)newpath, 0);
}



int dup2(int oldfd, int newfd) {
    return (int)__syscall(SYS_dup2, oldfd, newfd, 0);
}

int getcwd(char *buf, int max) {
    return (int)__syscall(SYS_getcwd, (long)buf, max, 0);
}

int ioctl(int fd, int cmd, uint64 arg) {
    return (int)__syscall(SYS_ioctl, fd, cmd, arg);
}
