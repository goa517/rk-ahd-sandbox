// Package encode 封装 mpp H.265 硬件编码器（rkvenc），dma-buf 导入零拷贝。
package encode

/*
#cgo CFLAGS: -Wall -O2 -I/usr/include/rockchip
#cgo LDFLAGS: -lrockchip_mpp
#include <stdlib.h>
#include "mppenc.h"
*/
import "C"

import (
	"errors"
	"fmt"
	"sync"
	"unsafe"
)

// 像素格式
const (
	FormatNV12 = iota
	FormatUYVY
)

var ErrNoOutput = errors.New("编码器无输出")

// Encoder 为一路 H.265 硬编通道。Encode 由采集 goroutine 单线程调用；
// SetRC/RequestIDR 可被其他 goroutine 调用（内部加锁）。
type Encoder struct {
	e  *C.mppenc_t
	mu sync.Mutex
}

// New 创建编码器。horStride 取 V4L2 驱动返回的 bytesperline，verStride 取 height。
func New(width, height, horStride, verStride, format, fps, bps, gop int) (*Encoder, error) {
	fmtC := C.mppenc_fmt_nv12()
	if format == FormatUYVY {
		fmtC = C.mppenc_fmt_uyvy()
	}
	errBuf := make([]byte, 256)
	e := C.mppenc_create(C.int(width), C.int(height), C.int(horStride), C.int(verStride),
		fmtC, C.int(fps), C.int(bps), C.int(gop),
		(*C.char)(unsafe.Pointer(&errBuf[0])), C.int(len(errBuf)))
	if e == nil {
		return nil, fmt.Errorf("创建编码器失败: %s", C.GoString((*C.char)(unsafe.Pointer(&errBuf[0]))))
	}
	return &Encoder{e: e}, nil
}

// AddBuffer 将 V4L2 导出的 dma-buf 注册为编码输入槽位。
func (en *Encoder) AddBuffer(slot, dmabufFD, size int) error {
	en.mu.Lock()
	defer en.mu.Unlock()
	if rc := C.mppenc_add_buffer(en.e, C.int(slot), C.int(dmabufFD), C.int(size)); rc != 0 {
		return fmt.Errorf("导入 dma-buf 失败 slot=%d", slot)
	}
	return nil
}

// Encode 编码一帧，返回 Annex-B 访问单元（含 IDR 前的 VPS/SPS/PPS）及该 AU
// 对应输入帧的 pts（编码器可能有流水线延迟，AU 不一定对应当次输入）。
func (en *Encoder) Encode(slot int, pts int64) ([]byte, int64, error) {
	en.mu.Lock()
	defer en.mu.Unlock()
	var outData *C.uint8_t
	var outLen C.int
	var outPts C.int64_t
	rc := C.mppenc_encode_slot(en.e, C.int(slot), C.int64_t(pts), &outData, &outLen, &outPts)
	if rc == 1 {
		return nil, 0, ErrNoOutput
	}
	if rc != 0 {
		return nil, 0, fmt.Errorf("编码失败 rc=%d", rc)
	}
	if outLen == 0 {
		return nil, 0, ErrNoOutput
	}
	return C.GoBytes(unsafe.Pointer(outData), outLen), int64(outPts), nil
}

// SetRC 运行时调整码率（kbps）与 GOP，不断流。
func (en *Encoder) SetRC(bitrateKbps, gop int) error {
	en.mu.Lock()
	defer en.mu.Unlock()
	if rc := C.mppenc_set_rc(en.e, C.int(bitrateKbps*1000), C.int(gop)); rc != 0 {
		return errors.New("MPP_ENC_SET_CFG 设置码率失败")
	}
	return nil
}

// RequestIDR 请求下一帧输出 IDR（新观众接入/丢包恢复用）。
func (en *Encoder) RequestIDR() {
	en.mu.Lock()
	defer en.mu.Unlock()
	C.mppenc_request_idr(en.e)
}

func (en *Encoder) Close() {
	en.mu.Lock()
	defer en.mu.Unlock()
	if en.e != nil {
		C.mppenc_destroy(en.e)
		en.e = nil
	}
}
