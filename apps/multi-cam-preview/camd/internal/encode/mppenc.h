#ifndef MPPENC_H
#define MPPENC_H

#include <stdint.h>

typedef struct mppenc mppenc_t;

int mppenc_fmt_nv12(void);
int mppenc_fmt_uyvy(void);

/* 创建 H.265 编码器（mpp rkvenc）。失败返回 NULL，错误信息写入 err */
mppenc_t *mppenc_create(int width, int height, int hor_stride, int ver_stride,
                        int fmt, int fps, int bps, int gop, char *err, int err_len);
/* 将一路 dma-buf 导入为编码器输入帧槽位（每个 V4L2 buffer 调用一次）。返回 0 或 -1 */
int mppenc_add_buffer(mppenc_t *e, int slot, int dmabuf_fd, int size);
/* 编码某槽位的帧（阻塞至拿到输出包）。
   返回 0 且有输出（*out_len>0），1 无输出，<0 错误。输出指针在下一次调用前有效。
   有输出时 *out_pts 为该码流包对应输入帧的 pts（编码器内部可能有流水线延迟，
   输出不一定对应当次输入帧，调用方应按 out_pts 匹配元数据） */
int mppenc_encode_slot(mppenc_t *e, int slot, int64_t pts,
                       uint8_t **out_data, int *out_len, int64_t *out_pts);
/* 运行时调整码率（bps）与 GOP，不断流。返回 0 或 -1 */
int mppenc_set_rc(mppenc_t *e, int bps, int gop);
/* 请求下一帧编码为 IDR */
int mppenc_request_idr(mppenc_t *e);
void mppenc_destroy(mppenc_t *e);

#endif
