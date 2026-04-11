#include "user.h"
#include "fcntl.h"

#define MAXLINE 256
#define MAXARGS 16
#define MAXCMDS 32
#define STDIN_FILENO 0
#define STDOUT_FILENO 1

// 判断字符是否为空白（空格或制表符）。
static int isspace1(char c) {
    return c == ' ' || c == '\t';
}

// 跳过字符串开头的连续空白字符，返回首个非空白位置。
static char *skipspace(char *s) {
    while (*s && isspace1(*s)) {
        s++;
    }
    return s;
}

// 去掉字符串末尾的连续空白字符（原地修改）。
static void rstrip(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && isspace1(s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
}

// 从标准输入读取一行到 buf，读到换行结束，不包含 '\n'。
static int readline(char *buf, int cap) {
    int i = 0;
    while (i + 1 < cap) {
        char c = 0;
        int n = read(0, &c, 1);
        if (n <= 0) {
            return -1;
        }
        if (c == '\r') {
            c = '\n';
        }
        if (c == '\n') {
            buf[i] = '\0';
            return 0;
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return 0;
}

// 将命令名转换为可执行路径：相对名补前缀 '/'，绝对路径保持不变。
static int makepath(const char *cmd, char *path, int cap) {
    int i = 0;
    if (cmd[0] != '/') {
        if (cap < 2) {
            return -1;
        }
        path[i++] = '/';
    }
    for (int j = 0; cmd[j] != '\0'; j++) {
        if (i + 1 >= cap) {
            return -1;
        }
        path[i++] = cmd[j];
    }
    path[i] = '\0';
    return 0;
}

// 按 ';' 分割多条命令，写入 cmds[] 并返回数量（会原地修改 line）。
static int split_commands(char *line, char *cmds[], int maxcmds) {
    int n = 0;
    char *p = line;
    while (*p && n < maxcmds) {
        while (*p == ';' || isspace1(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        cmds[n++] = p;
        while (*p && *p != ';') {
            p++;
        }
        if (*p == ';') {
            *p = '\0';
            p++;
        }
    }
    cmds[n] = 0;
    return n;
}



// 按空白分割参数，生成 argv 数组并以 NULL 结尾。
static int parseargs(char *line, char *argv[], int maxargs) {
    int argc = 0;
    char *s = skipspace(line);
    rstrip(s);

    while (*s) {
        if (argc + 1 >= maxargs) {
            return -1;
        }
        argv[argc++] = s;
        while (*s && !isspace1(*s)) {
            s++;
        }
        if (*s == '\0') {
            break;
        }
        *s = '\0';
        s = skipspace(s + 1);
    }
    argv[argc] = 0;
    return argc;
}

// 处理重定向符号（>, >>, <）：从 argv 中移除重定向参数并设置输入/输出 fd。
static int process_redirect(int argc, char *argv[], int fd[2]) {
    fd[0] = STDIN_FILENO;
    fd[1] = STDOUT_FILENO;
    int j = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], ">") == 0) {
            if (i + 1 >= argc) {
                break;
            }
            //TODO: 使用open函数打开文件，并设置正确的打开模式（替换此处的-1）
            int tfd = open(argv[i + 1], O_WRONLY | O_CREATE | O_TRUNC);
            if (tfd >= 0) {
                fd[1] = tfd;
            } else {
                printf("sh: open '%s' error\n", argv[i + 1]);
                return -1;
            }
            i++;
        } else if (strcmp(argv[i], ">>") == 0) {
            if (i + 1 >= argc) {
                break;
            }
            //TODO: 使用open函数打开文件，并设置正确的打开模式（替换此处的-1）
            int tfd = open(argv[i + 1], O_WRONLY | O_CREATE | O_APPEND);
            if (tfd >= 0) {
                fd[1] = tfd;
            } else {
                printf("sh: open '%s' error\n", argv[i + 1]);
                return -1;
            }
            i++;
        } else if (strcmp(argv[i], "<") == 0) {
            if (i + 1 >= argc) {
                break;
            }
            //TODO: 使用open函数打开文件，并设置正确的打开模式（替换此处的-1）
            int tfd = open(argv[i + 1], O_RDONLY);
            if (tfd >= 0) {
                fd[0] = tfd;
            } else {
                printf("sh: open '%s' error\n", argv[i + 1]);
                return -1;
            }
            i++;
        } else {
            argv[j++] = argv[i];
        }
    }
    argv[j] = 0;
    return j;
}

// 执行内建命令（help/cd/exit），返回 0 表示已处理，-1 表示非内建。
static int run_builtin(int argc, char *argv[]) {
    if (argc == 0) {
        return 0;
    }
    if (strcmp(argv[0], "help") == 0) {
        printf("Run program by name with args, e.g. sleep 20, echo hi\n");
        printf("Builtins: cd [dir], exit. Use ; for multiple commands.\n");
        printf("Redirect: >, >>, <\n");
        return 0;
    }
    if (strcmp(argv[0], "cd") == 0) {
        const char *target = "/";
        if (argc > 2) {
            printf("[sh] usage: cd [dir]\n");
            return 0;
        }
        if (argc == 2) {
            target = argv[1];
        }
        if (chdir(target) < 0) {
            printf("[sh] cd failed\n");
        }
        return 0;
    }
    if (strcmp(argv[0], "exit") == 0) {
        exit(0);
    }
    return -1;
}

// Shell 主循环：读取输入、解析参数、处理重定向、执行内建或 fork+exec 外部命令。
int main(void) {
    char line[MAXLINE];
    char path[MAXLINE];
    char *argv[MAXARGS];

    for (;;) {
        char cwd[128];
        if (getcwd(cwd, sizeof(cwd)) < 0) {
            cwd[0] = '/';
            cwd[1] = '\0';
        }
        printf("shell:%s -> ", cwd);

        if (readline(line, sizeof(line)) < 0) {
            printf("\n[sh] input closed\n");
            exit(0);
        }
        //TODO: 在这里分割多指令，并循环按照下面流程顺序处理每条指令
        char *cmds[MAXCMDS + 1];
        int ncmd = split_commands(line, cmds, MAXCMDS);//split commands
        for (int ci = 0; ci < ncmd; ci++) {//exec every command
            char *cmdline = skipspace(cmds[ci]);
            rstrip(cmdline);
            if (cmdline[0] == '\0') {
                continue;
            }

            int argc = parseargs(cmdline, argv, MAXARGS);

            if (argc < 0) {
                printf("[sh] too many args\n");
                continue;
            }
            if (argc == 0) {
                continue;
            }

            int fd[2];
            argc = process_redirect(argc, argv, fd);
            if (argc < 0) {
                printf("[sh] redirect failed\n");
                continue;
            }
            if (run_builtin(argc, argv) == 0) {
                if (fd[0] != STDIN_FILENO) {
                    close(fd[0]);
                }
                if (fd[1] != STDOUT_FILENO) {
                    close(fd[1]);
                }
                continue;
            }

            char *cmd = argv[0];
            if (makepath(cmd, path, sizeof(path)) < 0) {
                printf("[sh] command too long\n");
                if (fd[0] != STDIN_FILENO) close(fd[0]);
                if (fd[1] != STDOUT_FILENO) close(fd[1]);
                continue;
            }
            argv[0] = path;

            int pid = fork();
            if (pid < 0) {
                printf("[sh] fork failed\n");
                if (fd[0] != STDIN_FILENO) close(fd[0]);
                if (fd[1] != STDOUT_FILENO) close(fd[1]);
                continue;
            }
            if (pid == 0) {
                dup2(fd[0], STDIN_FILENO);
                dup2(fd[1], STDOUT_FILENO);
                if (fd[0] != STDIN_FILENO) close(fd[0]);
                if (fd[1] != STDOUT_FILENO) close(fd[1]);
                exec(path, argv);
                printf("[sh] exec failed\n");
                exit(127);
            }

            if (fd[0] != STDIN_FILENO) close(fd[0]);
            if (fd[1] != STDOUT_FILENO) close(fd[1]);

            int status = 0;
            if (wait(&status) < 0) {
                printf("[sh] wait failed\n");
            }
        }
    }
}
