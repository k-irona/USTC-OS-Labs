#include "user.h"
#include "types.h"
#include "fcntl.h"
#include "gpu.h"

int main(void) {
    int fd = open("/gpu", O_RDWR);
    if (fd < 0) {
        fprintf(2, "gpu_stats: failed to open /gpu\n");
        exit(1);
    }//open failed

    struct gpu_stats st;
    if (ioctl(fd, GPU_IOC_GET_STATS, (uint64)&st) < 0) {
        fprintf(2, "gpu_stats: GPU_IOC_GET_STATS failed\n");
        close(fd);
        exit(1);
    }//get failed

    printf("gpu matmul_ops: %llu\n", (unsigned long long)st.matmul_ops);

    close(fd);
    exit(0);
}