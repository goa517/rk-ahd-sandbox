#ifndef V4L2CAP_H
#define V4L2CAP_H

#include <stdint.h>

#define V4L2CAP_MAX_BUFS 32

typedef struct {
    int      fd;
    int      width;
    int      height;
    uint32_t pixfmt;
    uint32_t bytesperline;
    int      nbufs;
    int      dmabuf_fd[V4L2CAP_MAX_BUFS];
    uint32_t buf_size[V4L2CAP_MAX_BUFS];
    char     card[32];   /* VIDIOC_QUERYCAP: 设备名 */
    char     driver[16]; /* VIDIOC_QUERYCAP: 驱动名 */
    char     bus[32];    /* VIDIOC_QUERYCAP: 总线信息 */
} v4l2cap_t;

uint32_t v4l2cap_fmt_nv12(void);
uint32_t v4l2cap_fmt_uyvy(void);

/* 打开设备、设置格式、申请并导出 dma-buf。返回 0 或 -errno */
int  v4l2cap_open(v4l2cap_t *cap, const char *dev, int width, int height,
                  uint32_t pixfmt, int nbufs);
/* 入队全部 buffer 并 STREAMON。返回 0 或 -errno */
int  v4l2cap_start(v4l2cap_t *cap);
/* 出队一帧。返回 index+1（>0）成功，0 超时，<0 为 -errno。
   ts_us 非空时写入该帧采集时刻（CLOCK_REALTIME 微秒，由 buffer 时间戳换算） */
int  v4l2cap_dqbuf(v4l2cap_t *cap, int timeout_ms, int64_t *ts_us);
/* 归还 buffer。返回 0 或 -errno */
int  v4l2cap_qbuf(v4l2cap_t *cap, int index);
/* 读取槽位对应的 dma-buf fd / buffer 大小 */
int      v4l2cap_get_fd(v4l2cap_t *cap, int index);
uint32_t v4l2cap_get_size(v4l2cap_t *cap, int index);
/* 隔点采样计算帧亮度（Y）均值，诊断 AE 震荡用。返回 0 或 -errno */
int      v4l2cap_luma_mean(v4l2cap_t *cap, int index, double *mean);
/* STREAMOFF、释放 buffer、关闭 fd */
void v4l2cap_close(v4l2cap_t *cap);

#endif
