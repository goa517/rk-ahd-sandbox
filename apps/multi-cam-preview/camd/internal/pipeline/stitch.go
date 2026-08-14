// stitch 通道实现：从 sources 声明的若干采集通道 tap 原始帧（dma-buf），
// RGA 逐源 裁剪/旋转/缩放 blit 到同一张 NV12 画布，再送 mpp H.265 编码。
// 画布与采集帧全程 dma-buf 零拷贝；RGA 与编码器均为硬核，CPU 只做调度。
package pipeline

import (
	"errors"
	"fmt"
	"log/slog"
	"time"

	"multi-cam-preview/camd/internal/capture"
	"multi-cam-preview/camd/internal/encode"
	"multi-cam-preview/camd/internal/stitch"
)

const (
	canvasBufs         = 4 // 画布 dma-buf 数量（编码器输入槽位轮转）
	srcParamPollPeriod = 500 * time.Millisecond
)

// stitchSourceState 为一路拼接源的运行时状态。
type stitchSourceState struct {
	ch *Channel
	q  chan RawFrame
	// 源采集参数（上线后缓存，逐帧校验以感知源端重建）
	w, h, stride, format int
}

// stitchState 为 stitch 通道的运行时资源。
type stitchState struct {
	srcs   []*stitchSourceState
	canvas *stitch.Canvas
	stride int // 画布行字节（16 对齐）
}

// captureParams 返回通道采集参数（拼接源用）；未上线时 ok=false。
func (ch *Channel) captureParams() (w, h, stride, format int, ok bool) {
	ch.opMu.Lock()
	defer ch.opMu.Unlock()
	if ch.cap == nil {
		return 0, 0, 0, 0, false
	}
	format = capture.FormatNV12
	if ch.snapshot().Format == "UYVY" {
		format = capture.FormatUYVY
	}
	return ch.cap.Width(), ch.cap.Height(), ch.cap.Stride(), format, true
}

func (ch *Channel) startStitchPipeline() error {
	ch.opMu.Lock()
	defer ch.opMu.Unlock()

	cfg := ch.snapshot()
	n := len(cfg.Sources)
	if n == 0 {
		return errors.New("stitch 通道未配置 sources")
	}
	if len(cfg.Layout) != 0 && len(cfg.Layout) != n {
		return fmt.Errorf("layout 数量(%d)与 sources 数量(%d)不一致", len(cfg.Layout), n)
	}

	stc := &stitchState{}
	defer func() { // 失败路径回收
		if stc != nil {
			for _, s := range stc.srcs {
				s.ch.UnsubscribeRaw(s.q)
			}
			if stc.canvas != nil {
				stc.canvas.Free()
			}
		}
	}()

	for _, id := range cfg.Sources {
		sc := ch.mgr.Get(id)
		if sc == nil || sc == ch || sc.snapshot().Type == "stitch" {
			return fmt.Errorf("拼接源不可用: %s", id)
		}
		stc.srcs = append(stc.srcs, &stitchSourceState{ch: sc, q: sc.SubscribeRaw()})
	}

	stride := (cfg.Width + 15) &^ 15 // mpp 要求行对齐
	enc, err := encode.New(cfg.Width, cfg.Height, stride, cfg.Height,
		encode.FormatNV12, cfg.FPS, cfg.BitrateKbps*1000, cfg.GOP)
	if err != nil {
		return err
	}
	// 画布直接用 mpp 内部分配的输入 buffer（mpp 自选硬件兼容 heap，
	// 避免 dma-heap/system 散页 buffer 导入 rkvenc 失败），RGA 经 fd 写入
	size := stride * cfg.Height * 3 / 2
	fds, err := enc.AllocInput(canvasBufs, size)
	if err != nil {
		enc.Close()
		return err
	}
	slog.Info("拼接画布已分配", "id", ch.ID(), "fds", fds, "size", size)
	stc.canvas = stitch.WrapCanvas(fds, size)
	stc.stride = stride

	ch.stc = stc
	stc = nil // 所有权移交
	ch.enc = enc
	ch.rebuildFlag.Store(false)
	return nil
}

// layoutTile 返回第 i 路源的画布放置；未配置 layout 时按数量自动生成网格。
func (ch *Channel) layoutTile(i int) stitch.Rect {
	cfg := ch.snapshot()
	if len(cfg.Layout) > i {
		t := cfg.Layout[i]
		return stitch.Rect{X: t.X, Y: t.Y, W: t.W, H: t.H}
	}
	// 默认网格：4 源 2x2，其余按列优先铺满
	cols := 2
	if len(cfg.Sources) > 4 {
		cols = 3
	}
	rows := (len(cfg.Sources) + cols - 1) / cols
	tw, th := cfg.Width/cols, cfg.Height/rows
	return stitch.Rect{X: (i % cols) * tw, Y: (i / cols) * th, W: tw, H: th}
}

func (ch *Channel) layoutRotate(i int) int {
	cfg := ch.snapshot()
	if len(cfg.Layout) > i {
		return cfg.Layout[i].Rotate
	}
	return 0
}

func (ch *Channel) layoutCrop(i int, srcW, srcH int) stitch.Rect {
	cfg := ch.snapshot()
	if len(cfg.Layout) > i && len(cfg.Layout[i].Crop) == 4 {
		c := cfg.Layout[i].Crop
		return stitch.Rect{X: c[0], Y: c[1], W: c[2], H: c[3]}
	}
	return stitch.Rect{X: 0, Y: 0, W: srcW, H: srcH}
}

// recvLatest 取源通道最新一帧（阻塞至首帧），队列中更旧的帧直接释放。
func recvLatest(q chan RawFrame, timeout time.Duration, stopCh chan struct{}) (RawFrame, error) {
	var f RawFrame
	select {
	case f = <-q:
	case <-time.After(timeout):
		return RawFrame{}, errors.New("拼接源取帧超时（源离线或未出流）")
	case <-stopCh:
		return RawFrame{}, errStopped
	}
	for {
		select {
		case nf := <-q:
			f.Release()
			f = nf
		default:
			return f, nil
		}
	}
}

func (ch *Channel) stitchLoop() error {
	cfg := ch.snapshot()
	stc := ch.stc

	// 等待全部源上线并缓存采集参数
	for {
		if !ch.running.Load() {
			return errStopped
		}
		ready := true
		for _, s := range stc.srcs {
			w, h, stride, format, ok := s.ch.captureParams()
			if !ok {
				ready = false
				break
			}
			s.w, s.h, s.stride, s.format = w, h, stride, format
		}
		if ready {
			break
		}
		select {
		case <-time.After(srcParamPollPeriod):
		case <-ch.stopCh:
			return errStopped
		}
	}
	slog.Info("拼接源全部就绪", "id", ch.ID(), "sources", cfg.Sources)

	pendTs := make(map[int64]int64)
	var tick, encIdx uint64
	var blitErrs uint64

	for {
		if !ch.running.Load() {
			return errStopped
		}
		if ch.rebuildFlag.Load() {
			return errRebuild
		}

		frames := make([]RawFrame, len(stc.srcs))
		releaseAll := func() {
			for _, f := range frames {
				if f.refs != nil {
					f.Release()
				}
			}
		}
		for i, s := range stc.srcs {
			f, err := recvLatest(s.q, waitFrameTimeout, ch.stopCh)
			if err != nil {
				releaseAll()
				return err
			}
			frames[i] = f
			// 源端重建（离线/分辨率变化）→ 本通道一并重建
			if w, h, stride, _, ok := s.ch.captureParams(); !ok || w != s.w || h != s.h || stride != s.stride {
				releaseAll()
				return fmt.Errorf("拼接源 %s 离线或参数变更", cfg.Sources[i])
			}
		}

		if shouldEncode(tick, cfg.SourceFPS, cfg.FPS) {
			slot := int(tick % uint64(stc.canvas.Num()))
			dstFD := stc.canvas.FD(slot)
			for i, f := range frames {
				s := stc.srcs[i]
				if err := stitch.Blit(f.FD(), s.w, s.h, s.stride, s.format,
					ch.layoutCrop(i, s.w, s.h),
					dstFD, cfg.Width, cfg.Height, stc.stride,
					ch.layoutTile(i), ch.layoutRotate(i)); err != nil {
					blitErrs++
					if blitErrs%uint64(max(cfg.SourceFPS, 1)) == 1 {
						slog.Warn("RGA 拼接 blit 失败", "id", ch.ID(),
							"src", cfg.Sources[i], "err", err, "累计", blitErrs)
					}
				}
			}

			pendTs[int64(encIdx)] = frames[0].WallUs
			au, outPts, err := ch.enc.Encode(slot, int64(encIdx))
			if err != nil && !errors.Is(err, encode.ErrNoOutput) {
				releaseAll()
				return err
			}
			if len(au) > 0 {
				ts, ok := pendTs[outPts]
				if !ok {
					ts = frames[0].WallUs
				}
				ch.hub.Broadcast(au, isKeyframe(au), ts)
				ch.frames.Add(1)
				ch.bytes.Add(uint64(len(au)))
				encIdx++
				for k := range pendTs {
					if k <= outPts {
						delete(pendTs, k)
					}
				}
			}
		}
		releaseAll()
		tick++
	}
}
