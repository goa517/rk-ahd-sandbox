// Package stitch 封装 RGA 拼接：dma-heap 画布分配 + improcess 裁剪/旋转/缩放 blit。
// 画布为 dma-buf，可同时被 RGA 写入与 mpp 编码器导入，全程零拷贝。
package stitch

/*
#cgo CFLAGS: -Wall -O2 -I/usr/include/rga
#cgo LDFLAGS: -lrga
#include <stdlib.h>
#include "rgastitch.h"
*/
import "C"

import (
	"errors"
	"unsafe"
)

// 源像素格式
const (
	FormatNV12 = iota
	FormatUYVY
)

// Rect 为像素矩形。
type Rect struct{ X, Y, W, H int }

// Canvas 为一组轮流使用的拼接画布 dma-buf。
type Canvas struct {
	fds   []int
	size  int
	owned bool // true 时 Free 负责关闭 fd；false（如 mpp 内部分配的槽位）仅解引用
}

// AllocCanvas 分配 n 块 width×height（行字节 stride）的 NV12 画布，初始为黑。
func AllocCanvas(width, height, stride, n int) (*Canvas, error) {
	if n <= 0 {
		n = 4
	}
	// 注意：C.int 是 32 位，不能直接写 []int（64 位）的底层数组
	cfds := make([]C.int, n)
	for i := range cfds {
		cfds[i] = -1
	}
	var size C.int
	if rc := C.stitch_canvas_alloc(C.int(width), C.int(height), C.int(stride), C.int(n),
		&cfds[0], &size); rc != 0 {
		return nil, errors.New("dma-heap 画布分配失败")
	}
	fds := make([]int, n)
	for i, fd := range cfds {
		fds[i] = int(fd)
	}
	return &Canvas{fds: fds, size: int(size), owned: true}, nil
}

// WrapCanvas 包装外部分配的 dma-buf fd（如 mpp 内部输入槽位）为画布，
// 所有权不归 Canvas，Free 不会关闭 fd。
func WrapCanvas(fds []int, size int) *Canvas {
	return &Canvas{fds: fds, size: size}
}

func (c *Canvas) Num() int      { return len(c.fds) }
func (c *Canvas) Size() int     { return c.size }
func (c *Canvas) FD(slot int) int { return c.fds[slot] }

func (c *Canvas) Free() {
	if c == nil || len(c.fds) == 0 || !c.owned {
		return
	}
	C.stitch_canvas_free((*C.int)(unsafe.Pointer(&c.fds[0])), C.int(len(c.fds)))
}

// Blit 将 src 帧的 srcRect 区域经 rotate（顺时针 0/90/180/270）+ 缩放写入画布 dstRect。
// 同步调用（RGA 完成才返回）。
func Blit(srcFD, srcW, srcH, srcStride, srcFormat int, srcRect Rect,
	dstFD, dstW, dstH, dstStride int, dstRect Rect, rotate int) error {
	rc := C.stitch_blit(C.int(srcFD), C.int(srcW), C.int(srcH), C.int(srcStride), C.int(srcFormat),
		C.int(srcRect.X), C.int(srcRect.Y), C.int(srcRect.W), C.int(srcRect.H),
		C.int(dstFD), C.int(dstW), C.int(dstH), C.int(dstStride),
		C.int(dstRect.X), C.int(dstRect.Y), C.int(dstRect.W), C.int(dstRect.H), C.int(rotate))
	if rc != 0 {
		return &BlitError{Status: int(rc)}
	}
	return nil
}

// BlitError 携带 librga IM_STATUS 错误码（负值）。
type BlitError struct{ Status int }

func (e *BlitError) Error() string {
	return "RGA improcess 失败, IM_STATUS=" + itoa(e.Status)
}

func itoa(v int) string {
	if v == 0 {
		return "0"
	}
	neg := v < 0
	if neg {
		v = -v
	}
	var b [12]byte
	i := len(b)
	for v > 0 {
		i--
		b[i] = byte('0' + v%10)
		v /= 10
	}
	if neg {
		i--
		b[i] = '-'
	}
	return string(b[i:])
}
