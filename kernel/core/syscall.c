#include "types.h"
#include "defs.h"
#include "syscall.h"
#include "proc.h"
#include "file.h"
#include "fs.h"
#include "fcntl.h"
#include "param.h"
#include "gpu.h"
#include "log.h"

static uint64 sys_write(void) {
    struct proc *p = myproc();
    int fd = (int)p->trapframe->a0;
    uint64 va = p->trapframe->a1;
    int n = (int)p->trapframe->a2;

    if (fd < 0 || fd >= NOFILE || n < 0) {
        return (uint64)-1;
    }
    struct file *f = p->ofile[fd];
    if (f == 0) {
        return (uint64)-1;
    }

    return (uint64)filewrite(f, va, n);
}

static uint64 sys_read(void) {
    struct proc *p = myproc();
    int fd = (int)p->trapframe->a0;
    uint64 va = p->trapframe->a1;
    int n = (int)p->trapframe->a2;

    if (fd < 0 || fd >= NOFILE || n < 0) {
        return (uint64)-1;
    }
    struct file *f = p->ofile[fd];
    if (f == 0) {
        return (uint64)-1;
    }

    return (uint64)fileread(f, va, n);
}

static uint64 sys_yield(void) {
    yield();
    return 0;
}

static uint64 sys_exit(void) {
    int status = (int)myproc()->trapframe->a0;
    proc_exit(status);
    return 0; // not reached
}

static uint64 sys_sbrk(void) {
    struct proc *p = myproc();
    int n = (int)p->trapframe->a0;
    uint64 addr = p->sz;
    if (growproc(n) < 0) {
        return (uint64)-1;
    }
    return addr;
}

static uint64 sys_fork(void) {
    return (uint64)fork();
}

static uint64 sys_wait(void) {
    struct proc *p = myproc();
    uint64 addr = p->trapframe->a0;
    return (uint64)wait(addr);
}

static uint64 sys_exec(void) {
    struct proc *p = myproc();
    char path[MAXPATH];
    if (copyinstr(p->pagetable, path, p->trapframe->a0, sizeof(path)) < 0) {
        return (uint64)-1;
    }

    uint64 uargv = p->trapframe->a1;
    char argbuf[MAXARG][MAXPATH];
    char *kargv[MAXARG + 1];
    for (int i = 0; i <= MAXARG; i++) {
        kargv[i] = 0;
    }

    if (uargv != 0) {
        for (int i = 0; i < MAXARG; i++) {
            uint64 uptr = 0;
            if (copyin(p->pagetable, (char *)&uptr, uargv + (uint64)i * sizeof(uint64),
                       sizeof(uint64)) < 0) {
                return (uint64)-1;
            }
            if (uptr == 0) {
                kargv[i] = 0;
                break;
            }
            if (copyinstr(p->pagetable, argbuf[i], uptr, sizeof(argbuf[i])) < 0) {
                return (uint64)-1;
            }
            kargv[i] = argbuf[i];
            if (i == MAXARG - 1) {
                return (uint64)-1;
            }
        }
    }

    return (uint64)exec(path, kargv);
}

static uint64 sys_close(void) {
    struct proc *p = myproc();
    int fd = (int)p->trapframe->a0;

    if (fd < 0 || fd >= NOFILE) {
        return (uint64)-1;
    }
    struct file *f = p->ofile[fd];
    if (f == 0) {
        return (uint64)-1;
    }
    p->ofile[fd] = 0;
    fileclose(f);
    return 0;
}

static uint64 sys_fstat(void) {
    struct proc *p = myproc();
    int fd = (int)p->trapframe->a0;
    uint64 st = p->trapframe->a1;

    if (fd < 0 || fd >= NOFILE) {
        return (uint64)-1;
    }
    struct file *f = p->ofile[fd];
    if (f == 0) {
        return (uint64)-1;
    }
    return (uint64)filestat(f, st);
}

static uint64 sys_dup(void) {
    struct proc *p = myproc();
    int fd = (int)p->trapframe->a0;

    if (fd < 0 || fd >= NOFILE) {
        return (uint64)-1;
    }
    struct file *f = p->ofile[fd];
    if (f == 0) {
        return (uint64)-1;
    }

    for (int i = 0; i < NOFILE; i++) {
        if (p->ofile[i] == 0) {
            p->ofile[i] = filedup(f);
            if (p->ofile[i] == 0) {
                return (uint64)-1;
            }
            return (uint64)i;
        }
    }
    return (uint64)-1;
}

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int fdalloc(struct file *f);

static struct inode *create(const char *path, short type) {
    char name[DIRSIZ + 1];
    struct inode *dp = nameiparent(path, name);
    if (dp == 0) {
        return 0;
    }

    ilock(dp);
    short dtype = dp->type;
    iunlock(dp);
    if (dtype != T_DIR) {
        iput(dp);
        return 0;
    }

    struct inode *ip = dirlookup(dp, name, 0);
    if (ip != 0) {
        ilock(ip);
        short itype = ip->type;
        iunlock(ip);
        if (type == T_FILE && (itype == T_FILE || itype == T_DEVICE)) {
            iput(dp);
            return ip;
        }
        iput(ip);
        iput(dp);
        return 0;
    }

    ip = ialloc(dp->dev, type);
    if (ip == 0) {
        iput(dp);
        return 0;
    }

    ilock(ip);
    ip->type = type;
    ip->major = 0;
    ip->minor = 0;
    ip->nlink = 1;
    ip->size = 0;
    memset(ip->addrs, 0, sizeof(ip->addrs));
    iupdate(ip);
    iunlock(ip);

    if (type == T_DIR) {
        if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0) {
            goto fail;
        }
    }

    if (dirlink(dp, name, ip->inum) < 0) {
        goto fail;
    }

    if (type == T_DIR) {
        ilock(dp);
        dp->nlink++;
        iupdate(dp);
        iunlock(dp);
    }

    iput(dp);
    return ip;

fail:
    ilock(ip);
    ip->nlink = 0;
    iupdate(ip);
    iunlock(ip);
    iput(ip);
    iput(dp);
    return 0;
}

static uint64 sys_open(void) {
    struct proc *p = myproc();
    char path[MAXPATH];
    int omode = (int)p->trapframe->a1;

    if (copyinstr(p->pagetable, path, p->trapframe->a0, sizeof(path)) < 0) {
        return (uint64)-1;
    }

    struct file *f = filealloc();
    if (f == 0) {
        return (uint64)-1;
    }

    int fd = fdalloc(f);
    if (fd < 0) {
        fileclose(f);
        return (uint64)-1;
    }

    if (streq(path, "console")) {
        f->type = FD_CONSOLE;
        f->readable = !(omode & O_WRONLY);
        f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
        if (f->readable == 0 && f->writable == 0) {
            p->ofile[fd] = 0;
            fileclose(f);
            return (uint64)-1;
        }
        return (uint64)fd;
    }

    struct inode *ip = 0;
    if (omode & O_CREATE) {
        ip = create(path, T_FILE);
    } else {
        ip = namei(path);
    }
    if (ip == 0) {
        p->ofile[fd] = 0;
        fileclose(f);
        return (uint64)-1;
    }

    ilock(ip);
    short type = ip->type;
    short major = ip->major;
    iunlock(ip);
    if (type == T_DIR && ((omode & O_WRONLY) || (omode & O_RDWR))) {
        iput(ip);
        p->ofile[fd] = 0;
        fileclose(f);
        return (uint64)-1;
    }

    if (type == T_DEVICE) {
        if (major == GPU_DEV_MAJOR) {
            f->type = FD_GPU;
            f->readable = !(omode & O_WRONLY);
            f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
            iput(ip);
            if (f->readable == 0 && f->writable == 0) {
                p->ofile[fd] = 0;
                fileclose(f);
                return (uint64)-1;
            }
            return (uint64)fd;
        }

        // 未实现的其他设备
        iput(ip);
        p->ofile[fd] = 0;
        fileclose(f);
        return (uint64)-1;
    }

    if ((omode & O_TRUNC) && type == T_FILE) {
        itrunc(ip);
    }

    f->type = FD_INODE;
    f->ip = ip;
    f->off = (omode & O_APPEND) && type == T_FILE ? ip->size : 0;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
    if (f->readable == 0 && f->writable == 0) {
        p->ofile[fd] = 0;
        fileclose(f);
        return (uint64)-1;
    }

    return (uint64)fd;
}

static uint64 sys_chdir(void) {
    struct proc *p = myproc();
    char path[MAXPATH];
    if (copyinstr(p->pagetable, path, p->trapframe->a0, sizeof(path)) < 0) {
        return (uint64)-1;
    }

    struct inode *ip = namei(path);
    if (ip == 0) {
        return (uint64)-1;
    }

    ilock(ip);
    short type = ip->type;
    iunlock(ip);
    if (type != T_DIR) {
        iput(ip);
        return (uint64)-1;
    }

    acquire(&p->lock);
    struct inode *old = p->cwd;
    p->cwd = ip;
    release(&p->lock);

    if (old) {
        iput(old);
    }
    return 0;
}

static uint64 sys_mkdir(void) {
    struct proc *p = myproc();
    char path[MAXPATH];
    if (copyinstr(p->pagetable, path, p->trapframe->a0, sizeof(path)) < 0) {
        return (uint64)-1;
    }

    struct inode *ip = create(path, T_DIR);
    if (ip == 0) {
        return (uint64)-1;
    }
    iput(ip);
    return 0;
}

static int isdirempty(struct inode *dp) {
    ilock(dp);
    if (dp->type != T_DIR) {
        iunlock(dp);
        return 0;
    }

    struct dirent de;
    for (uint off = 2 * sizeof(de); off + sizeof(de) <= dp->size; off += sizeof(de)) {
        int n = readi(dp, off, &de, sizeof(de));
        if (n != (int)sizeof(de)) {
            iunlock(dp);
            return 0;
        }
        if (de.inum != 0) {
            iunlock(dp);
            return 0;
        }
    }
    iunlock(dp);
    return 1;
}

static uint64 sys_unlink(void) {
    struct proc *p = myproc();
    char path[MAXPATH];
    if (copyinstr(p->pagetable, path, p->trapframe->a0, sizeof(path)) < 0) {
        return (uint64)-1;
    }

    char name[DIRSIZ + 1];
    struct inode *dp = nameiparent(path, name);
    if (dp == 0) {
        return (uint64)-1;
    }

    ilock(dp);
    short dptype = dp->type;
    iunlock(dp);
    if (dptype != T_DIR) {
        iput(dp);
        return (uint64)-1;
    }

    if (streq(name, ".") || streq(name, "..")) {
        iput(dp);
        return (uint64)-1;
    }

    uint off = 0;
    struct inode *ip = dirlookup(dp, name, &off);
    if (ip == 0) {
        iput(dp);
        return (uint64)-1;
    }

    ilock(ip);
    short iptype = ip->type;
    short nlink = ip->nlink;
    iunlock(ip);
    if (nlink < 1) {
        iput(ip);
        iput(dp);
        return (uint64)-1;
    }
    if (iptype == T_DIR && !isdirempty(ip)) {
        iput(ip);
        iput(dp);
        return (uint64)-1;
    }

    struct dirent de;
    memset(&de, 0, sizeof(de));
    if (writei(dp, off, &de, sizeof(de)) != (int)sizeof(de)) {
        iput(ip);
        iput(dp);
        return (uint64)-1;
    }

    if (iptype == T_DIR) {
        ilock(dp);
        if (dp->nlink > 0) {
            dp->nlink--;
        }
        iupdate(dp);
        iunlock(dp);
    }
    iput(dp);

    ilock(ip);
    ip->nlink--;
    iupdate(ip);
    iunlock(ip);
    iput(ip);

    return 0;
}

static uint64 sys_link(void) {
    struct proc *p = myproc();
    char oldpath[MAXPATH];
    char newpath[MAXPATH];

    if (copyinstr(p->pagetable, oldpath, p->trapframe->a0, sizeof(oldpath)) < 0 ||
        copyinstr(p->pagetable, newpath, p->trapframe->a1, sizeof(newpath)) < 0) {
        return (uint64)-1;
    }

    struct inode *ip = namei(oldpath);
    if (ip == 0) {
        return (uint64)-1;
    }

    ilock(ip);
    short type = ip->type;
    if (type == T_DIR) {
        iunlock(ip);
        iput(ip);
        return (uint64)-1;
    }
    ip->nlink++;
    iupdate(ip);
    iunlock(ip);

    char name[DIRSIZ + 1];
    struct inode *dp = nameiparent(newpath, name);
    if (dp == 0) {
        goto bad;
    }

    ilock(dp);
    short dptype = dp->type;
    iunlock(dp);
    if (dptype != T_DIR || dp->dev != ip->dev) {
        iput(dp);
        goto bad;
    }

    if (dirlink(dp, name, ip->inum) < 0) {
        iput(dp);
        goto bad;
    }

    iput(dp);
    iput(ip);
    return 0;

bad:
    ilock(ip);
    if (ip->nlink > 0) {
        ip->nlink--;
    }
    iupdate(ip);
    iunlock(ip);
    iput(ip);
    return (uint64)-1;
}

static uint64 sys_getpid(void) {
    struct proc *p = myproc();
    if (p == 0) {
        return (uint64)-1;
    }
    return (uint64)p->pid;
}

static uint64 sys_sleep(void) {
    struct proc *p = myproc();
    int n = (int)p->trapframe->a0;
    if (n < 0) {
        return (uint64)-1;
    }
    if (ticks_sleep((uint)n) < 0) {
        return (uint64)-1;
    }
    return 0;
}

static uint64 sys_uptime(void) {
    return (uint64)ticks_get();
}

static uint64 sys_kill(void) {
    struct proc *p = myproc();
    int pid = (int)p->trapframe->a0;
    return (uint64)kill(pid);
}

static int fdalloc(struct file *f) {
    struct proc *p = myproc();
    for (int i = 0; i < NOFILE; i++) {
        if (p->ofile[i] == 0) {
            p->ofile[i] = f;
            return i;
        }
    }
    return -1;
}

static uint64 sys_pipe(void) {
    struct proc *p = myproc();
    uint64 fdarray = p->trapframe->a0;
    struct file *rf = 0;
    struct file *wf = 0;
    int fd0 = -1;
    int fd1 = -1;

    if (pipealloc(&rf, &wf) < 0) {
        return (uint64)-1;
    }
    fd0 = fdalloc(rf);
    if (fd0 < 0) {
        fileclose(rf);
        fileclose(wf);
        return (uint64)-1;
    }
    fd1 = fdalloc(wf);
    if (fd1 < 0) {
        p->ofile[fd0] = 0;
        fileclose(rf);
        fileclose(wf);
        return (uint64)-1;
    }

    int fdpair[2];
    fdpair[0] = fd0;
    fdpair[1] = fd1;
    if (copyout(p->pagetable, fdarray, (char *)fdpair, sizeof(fdpair)) < 0) {
        p->ofile[fd0] = 0;
        p->ofile[fd1] = 0;
        fileclose(rf);
        fileclose(wf);
        return (uint64)-1;
    }
    return 0;
}

static uint64 sys_dup2(void) {
    struct proc *p = myproc();
    int oldfd = (int)p->trapframe->a0;
    int newfd = (int)p->trapframe->a1;

    if (oldfd < 0 || oldfd >= NOFILE || newfd < 0 || newfd >= NOFILE) {
        return (uint64)-1;
    }
    struct file *f = p->ofile[oldfd];
    if (f == 0) {
        return (uint64)-1;
    }
    if (oldfd == newfd) {
        return (uint64)newfd;
    }
    if (p->ofile[newfd] != 0) {
        fileclose(p->ofile[newfd]);
        p->ofile[newfd] = 0;
    }
    p->ofile[newfd] = filedup(f);
    return (uint64)newfd;
}

static uint64 sys_getcwd(void) {
    struct proc *p = myproc();
    char *buf = (char *)p->trapframe->a0;
    int max = (int)p->trapframe->a1;

    if (buf == 0 || max < 2) {
        return (uint64)-1;
    }
    struct inode *cwd = proc_cwddup();
    if (cwd == 0) {
        return (uint64)-1;
    }
    char kbuf[MAXPATH];
    if (getcwd_path(cwd, kbuf, sizeof(kbuf)) < 0) {
        iput(cwd);
        return (uint64)-1;
    }
    iput(cwd);
    int len = 0;
    while (kbuf[len] != '\0' && len < max - 1) {
        len++;
    }
    len++;
    if (copyout(p->pagetable, (uint64)buf, kbuf, (uint64)len) < 0) {
        return (uint64)-1;
    }
    return 0;
}

static uint64 sys_ioctl(void) {
    struct proc *p = myproc();
    int fd = (int)p->trapframe->a0;
    int cmd = (int)p->trapframe->a1;
    uint64 arg = p->trapframe->a2;

    if (fd < 0 || fd >= NOFILE) {
        return (uint64)-1;
    }

    struct file *f = p->ofile[fd];
    if (f == 0) {
        return (uint64)-1;
    }

    return (uint64)fileioctl(f, cmd, arg);
}



static uint64 (*syscalls[])(void) = {
    [SYS_write] = sys_write,
    [SYS_read] = sys_read,
    [SYS_yield] = sys_yield,
    [SYS_exit] = sys_exit,
    [SYS_sbrk] = sys_sbrk,
    [SYS_fork] = sys_fork,
    [SYS_wait] = sys_wait,
    [SYS_exec] = sys_exec,
    [SYS_close] = sys_close,
    [SYS_dup] = sys_dup,
    [SYS_open] = sys_open,
    [SYS_getpid] = sys_getpid,
    [SYS_sleep] = sys_sleep,
    [SYS_uptime] = sys_uptime,
    [SYS_kill] = sys_kill,
    [SYS_pipe] = sys_pipe,
    [SYS_fstat] = sys_fstat,
    [SYS_chdir] = sys_chdir,
    [SYS_mkdir] = sys_mkdir,
    [SYS_unlink] = sys_unlink,
    [SYS_link] = sys_link,
    [SYS_dup2] = sys_dup2,
    [SYS_getcwd] = sys_getcwd,
    [SYS_ioctl] = sys_ioctl,
};

void syscall(void) {
    struct proc *p = myproc();
    int num = (int)p->trapframe->a7;

    // if (num > 0 && num < (int)(sizeof(syscalls) / sizeof(syscalls[0])) && syscalls[num]) {
    //     p->trapframe->a0 = syscalls[num]();
    // } else {
    //     printf("Unknown syscall %d\n", num);
    //     p->trapframe->a0 = (uint64)-1;
    // }
    if(num > 0 && num < (int)((sizeof(syscalls)) / sizeof(syscalls[0])) && syscalls[num]) {
        // [埋点] 打印进程正在调用哪个系统调用
        // LOG_DEBUG("syscall %d executing", num); 
        p->trapframe->a0 = syscalls[num]();
    } else {
        LOG_WARN("unknown syscall %d", num);
        p->trapframe->a0 = (uint64)-1;
    }
}
