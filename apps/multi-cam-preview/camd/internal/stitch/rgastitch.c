#include "rgastitch.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include <im2d.h>

/* 画布像素格式固定 NV12（mpp H.265 编码直接消费） */
#define DST_FMT RK_FORMAT_YCbCr_420_SP

static int dma_heap_alloc(size_t size) {
    static const char *heaps[] = { "/dev/dma_heap/system", "/dev/dma_heap/reserved" };
    for (unsigned i = 0; i < sizeof(heaps) / sizeof(heaps[0]); i++) {
        int heap = open(heaps[i], O_RDWR | O_CLOEXEC);
        if (heap < 0)
            continue;
        struct dma_heap_allocation_data d;
        memset(&d, 0, sizeof(d));
        d.len = size;
        d.fd_flags = O_RDWR | O_CLOEXEC;
        int rc = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &d);
        close(heap);
        if (rc == 0)
            return d.fd;
    }
    return -1;
}

/* 初始化为 NV12 黑（Y=0x10, UV=0x80），并刷 CPU cache 保证 RGA 可见 */
static void nv12_fill_black(int fd, int stride, int height, size_t size) {
    uint8_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        return;
    memset(p, 0x10, (size_t)stride * (size_t)height);
    memset(p + (size_t)stride * (size_t)height, 0x80, (size_t)stride * (size_t)height / 2);
    munmap(p, size);
    struct dma_buf_sync sync = { DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW };
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

int stitch_canvas_alloc(int width, int height, int stride, int n, int *fds, int *size) {
    (void)width;
    size_t sz = (size_t)stride * (size_t)height * 3 / 2;
    int got = 0;
    for (int i = 0; i < n; i++) {
        int fd = dma_heap_alloc(sz);
        if (fd < 0) {
            stitch_canvas_free(fds, got);
            return -1;
        }
        nv12_fill_black(fd, stride, height, sz);
        fds[i] = fd;
        got++;
    }
    *size = (int)sz;
    return 0;
}

void stitch_canvas_free(int *fds, int n) {
    for (int i = 0; i < n; i++) {
        if (fds[i] >= 0)
            close(fds[i]);
        fds[i] = -1;
    }
}

int stitch_blit(int src_fd, int src_w, int src_h, int src_stride, int src_fmt,
                int sx, int sy, int sw, int sh,
                int dst_fd, int dst_w, int dst_h, int dst_stride,
                int dx, int dy, int dw, int dh, int rotate) {
    int sfmt = src_fmt == 1 ? RK_FORMAT_UYVY_422 : RK_FORMAT_YCbCr_420_SP;
    rga_buffer_t src = wrapbuffer_fd_t(src_fd, src_w, src_h, src_stride, src_h, sfmt);
    rga_buffer_t dst = wrapbuffer_fd_t(dst_fd, dst_w, dst_h, dst_stride, dst_h, DST_FMT);
    rga_buffer_t pat;
    memset(&pat, 0, sizeof(pat));

    im_rect srect = { sx, sy, sw, sh };
    im_rect drect = { dx, dy, dw, dh };
    im_rect prect = { 0, 0, 0, 0 };

    int usage = 0;
    switch (rotate) {
    case 90:  usage = IM_HAL_TRANSFORM_ROT_90;  break;
    case 180: usage = IM_HAL_TRANSFORM_ROT_180; break;
    case 270: usage = IM_HAL_TRANSFORM_ROT_270; break;
    }

    IM_STATUS st = improcess(src, dst, pat, srect, drect, prect, usage);
    return st == IM_STATUS_SUCCESS ? 0 : (int)st;
}
