#include "user.h"
#include "fcntl.h"

#define NFILES 120
#define QUERY_BUF 4096
#define NAME_LEN 16

static char scan_buf[QUERY_BUF];
static char index_buf[QUERY_BUF];

static void fail(const char *msg) {
    printf("[bonusbench] FAIL: %s\n", msg);
    exit(1);
}

static void make_name(int i, char *out) {
    out[0] = '/';
    out[1] = 'b';
    out[2] = 'b';
    out[3] = (char)('0' + (i / 100) % 10);
    out[4] = (char)('0' + (i / 10) % 10);
    out[5] = (char)('0' + i % 10);
    out[6] = 0;
}

static void create_empty_file(const char *path) {
    int fd = open(path, O_CREATE | O_TRUNC | O_RDWR);
    if (fd < 0) {
        fail("create benchmark file");
    }
    if (close(fd) < 0) {
        fail("close benchmark file");
    }
}

static void prepare_dataset(void) {
    char path[NAME_LEN];
    for (int i = 0; i < NFILES; i++) {
        make_name(i, path);
        create_empty_file(path);
        if (i % 3 == 0) {
            if (setkeywords(path, "bcommon btarget") < 0) {
                fail("set target keywords");
            }
        } else {
            if (setkeywords(path, "bcommon bother") < 0) {
                fail("set filler keywords");
            }
        }
    }
}

static int streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static int run_scan_loop(int loops) {
    int start = uptime();
    for (int i = 0; i < loops; i++) {
        if (query_file("btarget", -1, scan_buf, sizeof(scan_buf)) < 0) {
            fail("full scan query");
        }
    }
    return uptime() - start;
}

static int run_index_loop(int loops) {
    int start = uptime();
    for (int i = 0; i < loops; i++) {
        if (query_file_indexed("btarget", -1, index_buf, sizeof(index_buf)) < 0) {
            fail("indexed query");
        }
    }
    return uptime() - start;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    prepare_dataset();

    int scan_n = query_file("btarget", -1, scan_buf, sizeof(scan_buf));
    int index_n = query_file_indexed("btarget", -1, index_buf, sizeof(index_buf));
    if (scan_n < 0 || index_n < 0) {
        fail("query returned error");
    }
    if (scan_n != index_n || scan_n != NFILES / 3) {
        fail("wrong result count");
    }
    if (!streq(scan_buf, index_buf)) {
        fail("indexed result differs from full scan");
    }

    int loops = 50;
    int scan_ticks = 0;
    int index_ticks = 0;
    while (loops <= 1600) {
        scan_ticks = run_scan_loop(loops);
        index_ticks = run_index_loop(loops);
        if (scan_ticks > 0 && index_ticks < scan_ticks) {
            break;
        }
        loops *= 2;
    }

    printf("[bonusbench] results=%d loops=%d full_scan_ticks=%d indexed_ticks=%d\n",
           scan_n, loops, scan_ticks, index_ticks);
    if (scan_ticks <= 0) {
        fail("full scan timing too small");
    }
    if (index_ticks >= scan_ticks) {
        fail("indexed query is not faster");
    }

    printf("[bonusbench] PASS\n");
    exit(0);
}
