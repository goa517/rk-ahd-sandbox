// Package capture 封装 V4L2 采集：MMAP 队列 + EXPBUF 导出 dma-buf，供 mpp 零拷贝消费。
package capture

/*
#cgo CFLAGS: -Wall -O2
#include <stdlib.h>
#include "v4l2cap.h"
*/
import "C"

import (
	"errors"
	"fmt"
	"syscall"
	"time"
	"unsafe"
)

// 像素格式
const (
	FormatNV12 = iota
	FormatUYVY
)

var ErrTimeout = errors.New("采集超时")

// Capture 代表一个已打开的 V4L2 采集会话。
type Capture struct {
	c   C.v4l2cap_t
	dev string
}

// Open 打开设备并配置格式/缓冲。nbufs 固定建议 >=16（vb2 队列耗尽会导致 rkisp 静默停帧）。
func Open(dev string, width, height, format, nbufs int) (*Capture, error) {
	if nbufs <= 0 || nbufs > C.V4L2CAP_MAX_BUFS {
		nbufs = 16
	}
	cdev := C.CString(dev)
	defer C.free(unsafe.Pointer(cdev))

	pixfmt := C.v4l2cap_fmt_nv12()
	if format == FormatUYVY {
		pixfmt = C.v4l2cap_fmt_uyvy()
	}

	c := &Capture{dev: dev}
	if rc := C.v4l2cap_open(&c.c, cdev, C.int(width), C.int(height), pixfmt, C.int(nbufs)); rc != 0 {
		return nil, fmt.Errorf("打开 %s 失败: %w", dev, syscall.Errno(-rc))
	}
	return c, nil
}

func (c *Capture) Start() error {
	if rc := C.v4l2cap_start(&c.c); rc != 0 {
		return fmt.Errorf("启动采集 %s 失败: %w", c.dev, syscall.Errno(-rc))
	}
	return nil
}

// WaitFrame 等待一帧到达，返回 buffer 槽位号与该帧采集时刻（墙上时钟 µs）。超时返回 ErrTimeout。
func (c *Capture) WaitFrame(timeout time.Duration) (int, int64, error) {
	var ts C.int64_t
	rc := C.v4l2cap_dqbuf(&c.c, C.int(timeout.Milliseconds()), &ts)
	if rc == 0 {
		return -1, 0, ErrTimeout
	}
	if rc < 0 {
		return -1, 0, fmt.Errorf("dqbuf %s: %w", c.dev, syscall.Errno(-rc))
	}
	return int(rc) - 1, int64(ts), nil
}

// Done 归还 buffer（编码完成后调用）。
func (c *Capture) Done(slot int) error {
	if rc := C.v4l2cap_qbuf(&c.c, C.int(slot)); rc != 0 {
		return fmt.Errorf("qbuf %s: %w", c.dev, syscall.Errno(-rc))
	}
	return nil
}

// DMAFD 返回某槽位的 dma-buf fd（会话期间有效）。
func (c *Capture) DMAFD(slot int) int { return int(C.v4l2cap_get_fd(&c.c, C.int(slot))) }

// BufSize 返回某槽位的 buffer 字节数。
func (c *Capture) BufSize(slot int) int { return int(C.v4l2cap_get_size(&c.c, C.int(slot))) }

func (c *Capture) Width() int   { return int(c.c.width) }
func (c *Capture) Height() int  { return int(c.c.height) }
func (c *Capture) Stride() int  { return int(c.c.bytesperline) }
func (c *Capture) NumBufs() int { return int(c.c.nbufs) }

// 设备能力信息（VIDIOC_QUERYCAP）
func (c *Capture) Card() string    { return C.GoString(&c.c.card[0]) }
func (c *Capture) Driver() string  { return C.GoString(&c.c.driver[0]) }
func (c *Capture) BusInfo() string { return C.GoString(&c.c.bus[0]) }

// LumaMean 隔点采样指定槽位帧的亮度（Y）均值，诊断 AE 震荡用。
func (c *Capture) LumaMean(slot int) (float64, error) {
	var m C.double
	if rc := C.v4l2cap_luma_mean(&c.c, C.int(slot), &m); rc != 0 {
		return 0, fmt.Errorf("luma %s: %w", c.dev, syscall.Errno(-rc))
	}
	return float64(m), nil
}

func (c *Capture) Close() { C.v4l2cap_close(&c.c) }
