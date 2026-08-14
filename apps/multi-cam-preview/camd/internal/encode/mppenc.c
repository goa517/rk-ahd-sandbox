#include "mppenc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/rk_venc_cmd.h>
#include <rockchip/rk_venc_rc.h>

#define MPPENC_MAX_SLOTS 32

struct mppenc {
    MppCtx         ctx;
    MppApi        *mpi;
    MppEncCfg      cfg;
    MppBufferGroup grp;      /* 内部分配槽位时的 buffer group */
    MppBuffer      buf[MPPENC_MAX_SLOTS];
    MppFrame       frame[MPPENC_MAX_SLOTS];
    int            nslots;
    int            width, height, hstride, vstride, fmt, fps, bps, gop;
    uint8_t       *out;
    size_t         out_cap;
};

int mppenc_fmt_nv12(void) { return MPP_FMT_YUV420SP; }
int mppenc_fmt_uyvy(void) { return MPP_FMT_YUV422_UYVY; }

static void set_err(char *err, int err_len, const char *fmt, ...) {
    if (!err || err_len <= 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static void set_rc_cfg(mppenc_t *e, int bps, int gop) {
    mpp_enc_cfg_set_s32(e->cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(e->cfg, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(e->cfg, "rc:bps_max", bps * 17 / 16);
    mpp_enc_cfg_set_s32(e->cfg, "rc:bps_min", bps * 15 / 16);
    mpp_enc_cfg_set_s32(e->cfg, "rc:gop", gop);
}

mppenc_t *mppenc_create(int width, int height, int hor_stride, int ver_stride,
                        int fmt, int fps, int bps, int gop, char *err, int err_len) {
    mppenc_t *e = calloc(1, sizeof(*e));
    if (!e) {
        set_err(err, err_len, "calloc failed");
        return NULL;
    }
    e->width = width;
    e->height = height;
    e->hstride = hor_stride;
    e->vstride = ver_stride;
    e->fmt = fmt;
    e->fps = fps;
    e->bps = bps;
    e->gop = gop;

    if (mpp_create(&e->ctx, &e->mpi) != MPP_OK) {
        set_err(err, err_len, "mpp_create failed");
        goto fail;
    }
    if (mpp_init(e->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC) != MPP_OK) {
        set_err(err, err_len, "mpp_init ENC/HEVC failed");
        goto fail;
    }

    if (mpp_enc_cfg_init(&e->cfg) != MPP_OK) {
        set_err(err, err_len, "mpp_enc_cfg_init failed");
        goto fail;
    }
    mpp_enc_cfg_set_s32(e->cfg, "prep:width", width);
    mpp_enc_cfg_set_s32(e->cfg, "prep:height", height);
    mpp_enc_cfg_set_s32(e->cfg, "prep:hor_stride", hor_stride);
    mpp_enc_cfg_set_s32(e->cfg, "prep:ver_stride", ver_stride);
    mpp_enc_cfg_set_s32(e->cfg, "prep:format", fmt);
    mpp_enc_cfg_set_s32(e->cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(e->cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(e->cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(e->cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(e->cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(e->cfg, "rc:fps_out_denom", 1);
    set_rc_cfg(e, bps, gop);

    if (e->mpi->control(e->ctx, MPP_ENC_SET_CFG, e->cfg) != MPP_OK) {
        set_err(err, err_len, "MPP_ENC_SET_CFG failed");
        goto fail;
    }

    MppEncHeaderMode hm = MPP_ENC_HEADER_MODE_EACH_IDR;
    if (e->mpi->control(e->ctx, MPP_ENC_SET_HEADER_MODE, &hm) != MPP_OK) {
        set_err(err, err_len, "MPP_ENC_SET_HEADER_MODE failed");
        goto fail;
    }
    MppEncSeiMode sm = MPP_ENC_SEI_MODE_DISABLE;
    e->mpi->control(e->ctx, MPP_ENC_SET_SEI_CFG, &sm);
    return e;

fail:
    mppenc_destroy(e);
    return NULL;
}

int mppenc_add_buffer(mppenc_t *e, int slot, int dmabuf_fd, int size) {
    if (!e || slot < 0 || slot >= MPPENC_MAX_SLOTS)
        return -1;

    MppBufferInfo info;
    memset(&info, 0, sizeof(info));
    info.type = MPP_BUFFER_TYPE_EXT_DMA;
    info.fd = dmabuf_fd;
    info.size = size;
    if (mpp_buffer_import(&e->buf[slot], &info) != MPP_OK)
        return -1;

    if (mpp_frame_init(&e->frame[slot]) != MPP_OK)
        return -1;
    MppFrame f = e->frame[slot];
    mpp_frame_set_width(f, e->width);
    mpp_frame_set_height(f, e->height);
    mpp_frame_set_hor_stride(f, e->hstride);
    mpp_frame_set_ver_stride(f, e->vstride);
    mpp_frame_set_fmt(f, e->fmt);
    mpp_frame_set_buffer(f, e->buf[slot]);

    if (slot + 1 > e->nslots)
        e->nslots = slot + 1;
    return 0;
}

/* 初始化某槽位的 MppFrame（公共部分） */
static int init_slot_frame(mppenc_t *e, int slot) {
    if (mpp_frame_init(&e->frame[slot]) != MPP_OK)
        return -1;
    MppFrame f = e->frame[slot];
    mpp_frame_set_width(f, e->width);
    mpp_frame_set_height(f, e->height);
    mpp_frame_set_hor_stride(f, e->hstride);
    mpp_frame_set_ver_stride(f, e->vstride);
    mpp_frame_set_fmt(f, e->fmt);
    mpp_frame_set_buffer(f, e->buf[slot]);
    if (slot + 1 > e->nslots)
        e->nslots = slot + 1;
    return 0;
}

int mppenc_alloc_input(mppenc_t *e, int nslots, int size, int *fds) {
    if (!e || nslots <= 0 || nslots > MPPENC_MAX_SLOTS)
        return -1;
    if (!e->grp && mpp_buffer_group_get_internal(&e->grp, MPP_BUFFER_TYPE_DRM) != MPP_OK)
        return -1;
    for (int i = 0; i < nslots; i++) {
        if (mpp_buffer_get(e->grp, &e->buf[i], size) != MPP_OK)
            return -1;
        fds[i] = mpp_buffer_get_fd(e->buf[i]);
        if (fds[i] < 0 || init_slot_frame(e, i) != 0)
            return -1;
    }
    return 0;
}

int mppenc_encode_slot(mppenc_t *e, int slot, int64_t pts,
                       uint8_t **out_data, int *out_len, int64_t *out_pts) {
    if (!e || slot < 0 || slot >= e->nslots)
        return -1;

    MppFrame f = e->frame[slot];
    mpp_frame_set_pts(f, pts);
    mpp_frame_set_eos(f, 0);

    if (e->mpi->encode_put_frame(e->ctx, f) != MPP_OK)
        return -2;

    MppPacket packet = NULL;
    if (e->mpi->encode_get_packet(e->ctx, &packet) != MPP_OK)
        return -3;
    if (!packet)
        return 1;

    if (out_pts)
        *out_pts = mpp_packet_get_pts(packet);

    size_t len = mpp_packet_get_length(packet);
    void *pos = mpp_packet_get_pos(packet);
    if (len > 0 && pos) {
        if (len > e->out_cap) {
            uint8_t *nb = realloc(e->out, len);
            if (!nb) {
                mpp_packet_deinit(&packet);
                return -4;
            }
            e->out = nb;
            e->out_cap = len;
        }
        memcpy(e->out, pos, len);
    }
    mpp_packet_deinit(&packet);

    *out_data = e->out;
    *out_len = (int)len;
    return 0;
}

int mppenc_set_rc(mppenc_t *e, int bps, int gop) {
    if (!e)
        return -1;
    e->bps = bps;
    e->gop = gop;
    set_rc_cfg(e, bps, gop);
    return e->mpi->control(e->ctx, MPP_ENC_SET_CFG, e->cfg) == MPP_OK ? 0 : -1;
}

int mppenc_request_idr(mppenc_t *e) {
    if (!e)
        return -1;
    return e->mpi->control(e->ctx, MPP_ENC_SET_IDR_FRAME, NULL) == MPP_OK ? 0 : -1;
}

void mppenc_destroy(mppenc_t *e) {
    if (!e)
        return;
    for (int i = 0; i < e->nslots; i++) {
        if (e->frame[i])
            mpp_frame_deinit(&e->frame[i]);
        if (e->buf[i])
            mpp_buffer_put(e->buf[i]);
    }
    if (e->cfg)
        mpp_enc_cfg_deinit(e->cfg);
    if (e->grp)
        mpp_buffer_group_put(e->grp);
    if (e->ctx)
        mpp_destroy(e->ctx);
    free(e->out);
    free(e);
}
