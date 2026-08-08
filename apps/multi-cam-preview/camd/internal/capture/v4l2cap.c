#include "v4l2cap.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

uint32_t v4l2cap_fmt_nv12(void) { return V4L2_PIX_FMT_NV12; }
uint32_t v4l2cap_fmt_uyvy(void) { return V4L2_PIX_FMT_UYVY; }

/* rkisp mainpath 为 multiplanar 节点（单 plane），统一走 MPLANE API */
#define BUF_TYPE V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static int qbuf_one(int fd, int index) {
    struct v4l2_plane plane;
    struct v4l2_buffer b;
    memset(&plane, 0, sizeof(plane));
    memset(&b, 0, sizeof(b));
    b.type = BUF_TYPE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = index;
    b.length = 1;
    b.m.planes = &plane;
    return xioctl(fd, VIDIOC_QBUF, &b);
}

int v4l2cap_open(v4l2cap_t *cap, const char *dev, int width, int height,
                 uint32_t pixfmt, int nbufs) {
    memset(cap, 0, sizeof(*cap));
    cap->fd = -1;

    int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    cap->fd = fd;

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = BUF_TYPE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = pixfmt;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
        goto fail;
    cap->width = (int)fmt.fmt.pix_mp.width;
    cap->height = (int)fmt.fmt.pix_mp.height;
    cap->pixfmt = fmt.fmt.pix_mp.pixelformat;
    cap->bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

    struct v4l2_capability vcap;
    memset(&vcap, 0, sizeof(vcap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &vcap) == 0) {
        snprintf(cap->card, sizeof(cap->card), "%s", vcap.card);
        snprintf(cap->driver, sizeof(cap->driver), "%s", vcap.driver);
        snprintf(cap->bus, sizeof(cap->bus), "%s", vcap.bus_info);
    }

    if (nbufs <= 0 || nbufs > V4L2CAP_MAX_BUFS)
        nbufs = 16;

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = nbufs;
    req.type = BUF_TYPE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
        goto fail;
    cap->nbufs = (int)req.count;

    for (int i = 0; i < cap->nbufs; i++) {
        struct v4l2_plane plane;
        struct v4l2_buffer b;
        memset(&plane, 0, sizeof(plane));
        memset(&b, 0, sizeof(b));
        b.type = BUF_TYPE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = 1;
        b.m.planes = &plane;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0)
            goto fail;
        cap->buf_size[i] = plane.length;

        struct v4l2_exportbuffer eb;
        memset(&eb, 0, sizeof(eb));
        eb.type = BUF_TYPE;
        eb.index = i;
        eb.plane = 0;
        eb.flags = O_RDWR | O_CLOEXEC;
        if (xioctl(fd, VIDIOC_EXPBUF, &eb) < 0)
            goto fail;
        cap->dmabuf_fd[i] = eb.fd;
    }
    return 0;

fail: {
        int e = -errno;
        v4l2cap_close(cap);
        return e;
    }
}

int v4l2cap_start(v4l2cap_t *cap) {
    for (int i = 0; i < cap->nbufs; i++) {
        if (qbuf_one(cap->fd, i) < 0)
            return -errno;
    }
    enum v4l2_buf_type type = BUF_TYPE;
    if (xioctl(cap->fd, VIDIOC_STREAMON, &type) < 0)
        return -errno;
    return 0;
}

int v4l2cap_dqbuf(v4l2cap_t *cap, int timeout_ms, int64_t *ts_us) {
    struct pollfd pfd = {cap->fd, POLLIN, 0};
    int r;
    do {
        r = poll(&pfd, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return -errno;
    if (r == 0)
        return 0; /* 超时 */

    struct v4l2_plane plane;
    struct v4l2_buffer b;
    memset(&plane, 0, sizeof(plane));
    memset(&b, 0, sizeof(b));
    b.type = BUF_TYPE;
    b.memory = V4L2_MEMORY_MMAP;
    b.length = 1;
    b.m.planes = &plane;
    if (xioctl(cap->fd, VIDIOC_DQBUF, &b) < 0) {
        if (errno == EAGAIN)
            return 0;
        return -errno;
    }
    if (ts_us) {
        int64_t us = (int64_t)b.timestamp.tv_sec * 1000000 + (int64_t)b.timestamp.tv_usec;
        /* rkisp 输出 CLOCK_MONOTONIC 时间戳，按当前偏移换算为墙上时钟；
           每帧重取偏移，天然跟随 NTP 校时 */
        if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
            struct timespec rt, mono;
            clock_gettime(CLOCK_REALTIME, &rt);
            clock_gettime(CLOCK_MONOTONIC, &mono);
            us += (int64_t)(rt.tv_sec - mono.tv_sec) * 1000000
                + (rt.tv_nsec - mono.tv_nsec) / 1000;
        }
        *ts_us = us;
    }
    return (int)b.index + 1;
}

int v4l2cap_qbuf(v4l2cap_t *cap, int index) {
    if (qbuf_one(cap->fd, index) < 0)
        return -errno;
    return 0;
}

int v4l2cap_get_fd(v4l2cap_t *cap, int index) {
    if (index < 0 || index >= cap->nbufs)
        return -1;
    return cap->dmabuf_fd[index];
}

uint32_t v4l2cap_get_size(v4l2cap_t *cap, int index) {
    if (index < 0 || index >= cap->nbufs)
        return 0;
    return cap->buf_size[index];
}

int v4l2cap_luma_mean(v4l2cap_t *cap, int index, double *mean) {
    if (index < 0 || index >= cap->nbufs)
        return -1;
    size_t sz = cap->buf_size[index];
    uint8_t *p = mmap(NULL, sz, PROT_READ, MAP_SHARED, cap->dmabuf_fd[index], 0);
    if (p == MAP_FAILED)
        return -errno;

    int uyvy = (cap->pixfmt == V4L2_PIX_FMT_UYVY);
    uint64_t sum = 0;
    uint32_t cnt = 0;
    for (int y = 0; y < cap->height; y += 16) {
        const uint8_t *row = p + (size_t)y * cap->bytesperline;
        for (int x = 0; x < cap->width; x += 16) {
            sum += uyvy ? row[x * 2] : row[x];
            cnt++;
        }
    }
    munmap(p, sz);
    if (!cnt)
        return -1;
    *mean = (double)sum / cnt;
    return 0;
}

void v4l2cap_close(v4l2cap_t *cap) {
    if (!cap || cap->fd < 0)
        return;
    enum v4l2_buf_type type = BUF_TYPE;
    xioctl(cap->fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < cap->nbufs; i++) {
        if (cap->dmabuf_fd[i] > 0)
            close(cap->dmabuf_fd[i]);
        cap->dmabuf_fd[i] = -1;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 0;
    req.type = BUF_TYPE;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(cap->fd, VIDIOC_REQBUFS, &req);

    close(cap->fd);
    cap->fd = -1;
}
