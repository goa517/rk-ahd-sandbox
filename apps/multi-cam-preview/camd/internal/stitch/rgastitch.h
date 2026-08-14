/* RGA 拼接画布：dma-heap 画布分配 + improcess 单源 blit（裁剪/旋转/缩放） */
#ifndef RGASTITCH_H
#define RGASTITCH_H

/* 分配 n 块 NV12 画布 dma-buf（stride 对齐由调用方给定），返回 0 成功。
 * fds 由调用方提供（长度 >= n），*size 返回单块字节数。 */
int stitch_canvas_alloc(int width, int height, int stride, int n, int *fds, int *size);
void stitch_canvas_free(int *fds, int n);

/* 将 src 的 (sx,sy,sw,sh) 区域经 rotate（顺时针 0/90/180/270）+ 缩放
 * blit 到 dst 的 (dx,dy,dw,dh) 区域。fmt: 0=NV12, 1=UYVY。
 * 同步调用，返回 0 成功，否则为 librga IM_STATUS 错误码（负值）。 */
int stitch_blit(int src_fd, int src_w, int src_h, int src_stride, int src_fmt,
                int sx, int sy, int sw, int sh,
                int dst_fd, int dst_w, int dst_h, int dst_stride,
                int dx, int dy, int dw, int dh, int rotate);

#endif
