// Package pipeline 编排每路通道：V4L2 采集 → mpp H.265 编码 → 订阅者广播。
// 编码输出通过 Hub 分发给 0..N 个 WebRTC 观众；参数变更时码率走运行时 set，
// 分辨率/帧率变更时重建采集+编码（Hub 与订阅关系保持，观众无感）。
package pipeline

import (
	"errors"
	"fmt"
	"log/slog"
	"os"
	"sync"
	"sync/atomic"
	"time"

	"multi-cam-preview/camd/internal/capture"
	"multi-cam-preview/camd/internal/config"
	"multi-cam-preview/camd/internal/encode"
)

const (
	captureBufs       = 16 // vb2 队列耗尽会导致 rkisp 静默停帧，不得小于 16
	subscriberQueue   = 60 // 每个观众的缓存帧数
	waitFrameTimeout  = 2 * time.Second
	retryStartDelay   = 5 * time.Second
	retryRestartDelay = 3 * time.Second
)

var (
	errStopped = errors.New("通道已停止")
	errRebuild = errors.New("参数变更，重建管线")
)

// Frame 为一帧编码输出（Annex-B AU）及其元数据。
type Frame struct {
	Data          []byte
	Keyframe      bool
	CaptureWallUs int64 // 采集时刻（CLOCK_REALTIME µs）
}

// Subscriber 为一个编码流订阅者（通常对应一个 WebRTC 观众）。
type Subscriber struct {
	C chan Frame
}

// Hub 为单通道的编码帧广播中心，生命周期独立于采集/编码管线。
type Hub struct {
	mu      sync.RWMutex
	subs    map[*Subscriber]struct{}
	lastIDR Frame
	hasIDR  bool
}

func NewHub() *Hub { return &Hub{subs: make(map[*Subscriber]struct{})} }

func (h *Hub) Broadcast(au []byte, keyframe bool, captureWallUs int64) {
	h.mu.Lock()
	defer h.mu.Unlock()
	f := Frame{Data: au, Keyframe: keyframe, CaptureWallUs: captureWallUs}
	if keyframe {
		h.lastIDR = f
		h.hasIDR = true
	}
	for sub := range h.subs {
		select {
		case sub.C <- f:
		default:
			// 慢消费者：丢最旧帧保最新，避免阻塞编码线程
			select {
			case <-sub.C:
			default:
			}
			select {
			case sub.C <- f:
			default:
			}
		}
	}
}

// Subscribe 返回订阅者与缓存的最近 IDR 帧（hasIDR 为 false 表示暂无），供新观众立即起播。
func (h *Hub) Subscribe() (*Subscriber, Frame, bool) {
	sub := &Subscriber{C: make(chan Frame, subscriberQueue)}
	h.mu.Lock()
	defer h.mu.Unlock()
	h.subs[sub] = struct{}{}
	return sub, h.lastIDR, h.hasIDR
}

func (h *Hub) Unsubscribe(sub *Subscriber) {
	h.mu.Lock()
	defer h.mu.Unlock()
	delete(h.subs, sub)
}

func (h *Hub) Viewers() int {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return len(h.subs)
}

// Channel 为一路摄像头通道。
type Channel struct {
	cfgMu sync.RWMutex
	cfg   config.Channel

	hub *Hub

	opMu        sync.Mutex // 保护 start/stop/SetRC 与 cap/enc 指针
	running     atomic.Bool
	rebuildFlag atomic.Bool
	online      atomic.Bool
	stopOnce    sync.Once
	stopCh      chan struct{}

	cap *capture.Capture
	enc *encode.Encoder

	frames  atomic.Uint64
	bytes   atomic.Uint64
	lastErr atomic.Value // string
	devInfo atomic.Value // devCaps，管线启动后有效
}

// devCaps 为采集设备的静态能力信息（V4L2 QUERYCAP + 实际协商参数）。
type devCaps struct {
	Card   string
	Driver string
	Bus    string
	Stride int
}

func NewChannel(cfg config.Channel) *Channel {
	return &Channel{
		cfg:    cfg,
		hub:    NewHub(),
		stopCh: make(chan struct{}),
	}
}

func (ch *Channel) ID() string { return ch.cfg.ID }

func (ch *Channel) snapshot() config.Channel {
	ch.cfgMu.RLock()
	defer ch.cfgMu.RUnlock()
	return ch.cfg
}

// Online 表示采集编码管线正在出流。
func (ch *Channel) Online() bool { return ch.online.Load() }

func (ch *Channel) Hub() *Hub { return ch.hub }

// FrameDuration 为编码帧间隔（WebRTC 采样时长用）。
func (ch *Channel) FrameDuration() time.Duration {
	fps := ch.snapshot().FPS
	if fps <= 0 {
		fps = 30
	}
	return time.Second / time.Duration(fps)
}

func (ch *Channel) setErr(err error) {
	if err != nil {
		ch.lastErr.Store(err.Error())
	}
}

// Start 启动通道监督 goroutine（自动重试/重建）。
func (ch *Channel) Start() {
	if !ch.running.CompareAndSwap(false, true) {
		return
	}
	go ch.supervise()
}

// Stop 永久停止通道。
func (ch *Channel) Stop() {
	ch.stopOnce.Do(func() {
		ch.running.Store(false)
		close(ch.stopCh)
	})
}

func (ch *Channel) supervise() {
	for ch.running.Load() {
		if err := ch.startPipeline(); err != nil {
			ch.setErr(err)
			ch.online.Store(false)
			slog.Warn("管线启动失败，稍后重试", "id", ch.ID(), "err", err)
			select {
			case <-time.After(retryStartDelay):
				continue
			case <-ch.stopCh:
				return
			}
		}
		ch.online.Store(true)
		cfg := ch.snapshot()
		slog.Info("管线已启动", "id", ch.ID(), "device", cfg.Device,
			"size", fmt.Sprintf("%dx%d@%d", cfg.Width, cfg.Height, cfg.FPS),
			"bitrate_kbps", cfg.BitrateKbps)

		err := ch.loop()
		ch.online.Store(false)
		ch.stopPipeline()

		if !ch.running.Load() {
			return
		}
		if errors.Is(err, errRebuild) {
			slog.Info("管线重建完成参数切换", "id", ch.ID())
			continue
		}
		ch.setErr(err)
		slog.Warn("管线异常退出，稍后重启", "id", ch.ID(), "err", err)
		select {
		case <-time.After(retryRestartDelay):
		case <-ch.stopCh:
			return
		}
	}
}

func (ch *Channel) startPipeline() error {
	ch.opMu.Lock()
	defer ch.opMu.Unlock()

	cfg := ch.snapshot()
	capFmt := capture.FormatNV12
	encFmt := encode.FormatNV12
	if cfg.Format == "UYVY" {
		capFmt = capture.FormatUYVY
		encFmt = encode.FormatUYVY
	}

	cap_, err := capture.Open(cfg.Device, cfg.Width, cfg.Height, capFmt, captureBufs)
	if err != nil {
		return err
	}
	enc, err := encode.New(cap_.Width(), cap_.Height(), cap_.Stride(), cap_.Height(),
		encFmt, cfg.FPS, cfg.BitrateKbps*1000, cfg.GOP)
	if err != nil {
		cap_.Close()
		return err
	}
	for i := 0; i < cap_.NumBufs(); i++ {
		if err := enc.AddBuffer(i, cap_.DMAFD(i), cap_.BufSize(i)); err != nil {
			enc.Close()
			cap_.Close()
			return err
		}
	}
	if err := cap_.Start(); err != nil {
		enc.Close()
		cap_.Close()
		return err
	}

	ch.cap = cap_
	ch.enc = enc
	ch.devInfo.Store(devCaps{
		Card: cap_.Card(), Driver: cap_.Driver(), Bus: cap_.BusInfo(),
		Stride: cap_.Stride(),
	})
	ch.rebuildFlag.Store(false)
	return nil
}

func (ch *Channel) stopPipeline() {
	ch.opMu.Lock()
	defer ch.opMu.Unlock()
	if ch.enc != nil {
		ch.enc.Close()
		ch.enc = nil
	}
	if ch.cap != nil {
		ch.cap.Close()
		ch.cap = nil
	}
}

func (ch *Channel) loop() error {
	cfg := ch.snapshot()
	srcFPS, encFPS := cfg.SourceFPS, cfg.FPS
	var frameIdx, encIdx uint64
	// 编码帧序号 → 采集时刻。编码器有流水线延迟时，输出 AU 按返回的 pts
	// 回查此处拿到正确的采集时间戳，避免错贴到当前帧上
	pendTs := make(map[int64]int64)
	var prevTs int64     // 上一 AU 的采集时间戳（单调性诊断）
	var dbgMiss, dbgLate int // pts 未命中 / 输出滞后计数（诊断）

	for {
		if !ch.running.Load() {
			return errStopped
		}
		if ch.rebuildFlag.Load() {
			return errRebuild
		}

		slot, wallUs, err := ch.cap.WaitFrame(waitFrameTimeout)
		if errors.Is(err, capture.ErrTimeout) {
			return errors.New("采集超时（疑似驱动停止出帧）")
		}
		if err != nil {
			return err
		}

		if shouldEncode(frameIdx, srcFPS, encFPS) {
			pendTs[int64(encIdx)] = wallUs
			au, outPts, err := ch.enc.Encode(slot, int64(encIdx))
			if err != nil && !errors.Is(err, encode.ErrNoOutput) {
				_ = ch.cap.Done(slot)
				return err
			}
			if len(au) > 0 {
				ts, ok := pendTs[outPts]
				if !ok {
					dbgMiss++
					ts = wallUs // 兜底：理论上不应发生
				} else if outPts != int64(encIdx) {
					dbgLate++ // 输出非当次输入（编码器流水线延迟）
				}
				if prevTs != 0 && ts < prevTs {
					slog.Warn("采集时间戳回退", "id", ch.ID(), "prev_us", prevTs, "cur_us", ts,
						"back_ms", (prevTs-ts)/1000)
				}
				// 帧间隔超过 1.5 倍标定周期 = sensor/ISP 丢帧（AE 调整曝光时常伴随）
				if gap := ts - prevTs; prevTs != 0 && gap > int64(ch.FrameDuration())*3/2 {
					slog.Info("采集帧间隔异常（疑似丢帧）", "id", ch.ID(), "gap_ms", gap/1000)
				}
				prevTs = ts
				ch.hub.Broadcast(au, isKeyframe(au), ts)
				ch.frames.Add(1)
				ch.bytes.Add(uint64(len(au)))
				encIdx++
				// 输出无重排（无 B 帧），≤outPts 的均为陈旧项
				for k := range pendTs {
					if k <= outPts {
						delete(pendTs, k)
					}
				}
				if encIdx%300 == 0 && (dbgMiss > 0 || dbgLate > 0) {
					slog.Info("编码器流水线诊断", "id", ch.ID(), "pts_miss", dbgMiss, "delayed_out", dbgLate)
					dbgMiss, dbgLate = 0, 0
				}
			}
		}
		// 每秒采样一次帧亮度（诊断：画面忽暗忽亮是采集端 AE 还是显示端问题）
		if frameIdx%uint64(max(srcFPS, 1)) == 0 {
			if m, err := ch.cap.LumaMean(slot); err == nil {
				slog.Info("亮度采样", "id", ch.ID(), "luma", int(m*10)/10.0)
			}
		}
		frameIdx++

		if err := ch.cap.Done(slot); err != nil {
			return err
		}
	}
}

// shouldEncode 按 encFPS/srcFPS 做帧抽取（采集帧率由 sensor 模式固定）。
func shouldEncode(idx uint64, srcFPS, encFPS int) bool {
	if encFPS >= srcFPS || encFPS <= 0 {
		return true
	}
	if idx == 0 {
		return true
	}
	return (idx*uint64(encFPS))/uint64(srcFPS) > ((idx-1)*uint64(encFPS))/uint64(srcFPS)
}

// IsKeyframe 导出供 WebRTC 起播过滤使用。
func IsKeyframe(au []byte) bool { return isKeyframe(au) }

// isKeyframe 扫描 Annex-B AU 的首个 VCL NAL 判断是否为 IDR。
func isKeyframe(au []byte) bool {
	n := len(au)
	for i := 0; i+4 < n; i++ {
		if au[i] != 0 || au[i+1] != 0 {
			continue
		}
		var hdr int
		if au[i+2] == 1 {
			hdr = i + 3
		} else if au[i+2] == 0 && au[i+3] == 1 {
			hdr = i + 4
		} else {
			continue
		}
		if hdr >= n {
			return false
		}
		ntype := (au[hdr] >> 1) & 0x3F
		if ntype == 19 || ntype == 20 { // IDR_W_RADL / IDR_N_LP
			return true
		}
		if ntype < 32 { // 首个 VCL 非 IDR
			return false
		}
		i = hdr
	}
	return false
}

// SetRC 运行时调整码率/GOP，不断流。
func (ch *Channel) SetRC(bitrateKbps, gop int) error {
	ch.opMu.Lock()
	defer ch.opMu.Unlock()
	if ch.enc == nil {
		return errors.New("编码器未运行")
	}
	return ch.enc.SetRC(bitrateKbps, gop)
}

// RequestIDR 请求编码器下一帧输出 IDR。
func (ch *Channel) RequestIDR() {
	ch.opMu.Lock()
	defer ch.opMu.Unlock()
	if ch.enc != nil {
		ch.enc.RequestIDR()
	}
}

// RequestRebuild 请求以最新配置重建采集+编码（Hub/订阅保持）。
func (ch *Channel) RequestRebuild() { ch.rebuildFlag.Store(true) }

// Stats 读取并清零周期计数之外的累计值（实际 fps/kbps 由调用方按间隔差分计算）。
func (ch *Channel) Stats() (frames, bytes uint64) {
	return ch.frames.Load(), ch.bytes.Load()
}

// Info 为通道对外状态视图。
type Info struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	Type        string `json:"type"`
	Device      string `json:"device,omitempty"`
	Enabled     bool   `json:"enabled"`
	Online      bool   `json:"online"`
	Width       int    `json:"width"`
	Height      int    `json:"height"`
	FPS         int    `json:"fps"`
	SourceFPS   int    `json:"source_fps"`
	Format      string `json:"format"`
	BitrateKbps int    `json:"bitrate_kbps"`
	GOP         int    `json:"gop"`
	Viewers     int    `json:"viewers"`
	LastError   string `json:"last_error,omitempty"`
	// 以下为采集设备信息，仅在线时有值
	Camera string `json:"camera,omitempty"`
	Driver string `json:"driver,omitempty"`
	Bus    string `json:"bus,omitempty"`
	Stride int    `json:"stride,omitempty"`
}

func (ch *Channel) Info() Info {
	cfg := ch.snapshot()
	lastErr, _ := ch.lastErr.Load().(string)
	info := Info{
		ID:          cfg.ID,
		Name:        cfg.Name,
		Type:        cfg.Type,
		Device:      cfg.Device,
		Enabled:     cfg.Enabled,
		Online:      ch.Online(),
		Width:       cfg.Width,
		Height:      cfg.Height,
		FPS:         cfg.FPS,
		SourceFPS:   cfg.SourceFPS,
		Format:      cfg.Format,
		BitrateKbps: cfg.BitrateKbps,
		GOP:         cfg.GOP,
		Viewers:     ch.hub.Viewers(),
		LastError:   lastErr,
	}
	if dc, ok := ch.devInfo.Load().(devCaps); ok && info.Online {
		info.Camera = dc.Card
		info.Driver = dc.Driver
		info.Bus = dc.Bus
		info.Stride = dc.Stride
	}
	return info
}

// ParamUpdate 描述一次参数变更，nil 字段表示不变。
type ParamUpdate struct {
	Width       *int `json:"width"`
	Height      *int `json:"height"`
	FPS         *int `json:"fps"`
	BitrateKbps *int `json:"bitrate_kbps"`
	GOP         *int `json:"gop"`
}

// Manager 管理全部通道。
type Manager struct {
	cfg      *config.Config
	channels []*Channel
	byID     map[string]*Channel
}

func NewManager(cfg *config.Config) *Manager {
	m := &Manager{cfg: cfg, byID: make(map[string]*Channel)}
	for _, c := range cfg.Channels {
		ch := NewChannel(c)
		m.channels = append(m.channels, ch)
		m.byID[c.ID] = ch
	}
	return m
}

// Start 启动所有可运行通道（raw/ahd 类型 + enabled + 设备存在）。
// ahd 通道（XS9922B 经 rkcif 直出 NV12/UYVY）与 raw 走同一条 V4L2 采集管线。
func (m *Manager) Start() {
	for _, ch := range m.channels {
		cfg := ch.snapshot()
		if !cfg.Enabled {
			continue
		}
		if cfg.Type != "raw" && cfg.Type != "ahd" {
			slog.Info("通道类型本期保留不实例化", "id", cfg.ID, "type", cfg.Type)
			continue
		}
		if cfg.Device == "" {
			continue
		}
		if _, err := os.Stat(cfg.Device); err != nil {
			slog.Warn("设备节点不存在，通道离线", "id", cfg.ID, "device", cfg.Device)
			ch.setErr(fmt.Errorf("设备不存在: %s", cfg.Device))
			continue
		}
		ch.Start()
	}
}

func (m *Manager) Stop() {
	for _, ch := range m.channels {
		ch.Stop()
	}
}

func (m *Manager) Get(id string) *Channel { return m.byID[id] }

func (m *Manager) Infos() []Info {
	infos := make([]Info, 0, len(m.channels))
	for _, ch := range m.channels {
		infos = append(infos, ch.Info())
	}
	return infos
}

// Update 应用参数变更：码率/GOP 动态生效；分辨率/帧率变更触发重建。
func (m *Manager) Update(id string, p ParamUpdate) (Info, error) {
	ch := m.byID[id]
	if ch == nil {
		return Info{}, fmt.Errorf("通道不存在: %s", id)
	}
	cfg := ch.snapshot()
	if cfg.Type != "raw" && cfg.Type != "ahd" {
		return ch.Info(), fmt.Errorf("通道类型 %s 暂不支持参数调整", cfg.Type)
	}

	newCfg := cfg
	if p.Width != nil {
		newCfg.Width = *p.Width
	}
	if p.Height != nil {
		newCfg.Height = *p.Height
	}
	if p.FPS != nil {
		newCfg.FPS = *p.FPS
	}
	if p.BitrateKbps != nil {
		newCfg.BitrateKbps = *p.BitrateKbps
	}
	if p.GOP != nil {
		newCfg.GOP = *p.GOP
	}

	if newCfg.Width < 320 || newCfg.Width > 2560 || newCfg.Width%2 != 0 {
		return ch.Info(), fmt.Errorf("非法宽度: %d", newCfg.Width)
	}
	if newCfg.Height < 240 || newCfg.Height > 1440 || newCfg.Height%2 != 0 {
		return ch.Info(), fmt.Errorf("非法高度: %d", newCfg.Height)
	}
	if newCfg.FPS < 1 || newCfg.FPS > newCfg.SourceFPS {
		return ch.Info(), fmt.Errorf("帧率需在 1~%d 之间", newCfg.SourceFPS)
	}
	if newCfg.BitrateKbps < 128 || newCfg.BitrateKbps > 20480 {
		return ch.Info(), fmt.Errorf("码率需在 128~20480 kbps 之间")
	}
	if newCfg.GOP < 1 || newCfg.GOP > 300 {
		return ch.Info(), fmt.Errorf("GOP 需在 1~300 之间")
	}

	needRebuild := newCfg.Width != cfg.Width || newCfg.Height != cfg.Height || newCfg.FPS != cfg.FPS
	rcChanged := newCfg.BitrateKbps != cfg.BitrateKbps || newCfg.GOP != cfg.GOP

	ch.cfgMu.Lock()
	ch.cfg = newCfg
	ch.cfgMu.Unlock()

	if needRebuild {
		ch.RequestRebuild()
	} else if rcChanged && ch.Online() {
		if err := ch.SetRC(newCfg.BitrateKbps, newCfg.GOP); err != nil {
			return ch.Info(), err
		}
	}
	return ch.Info(), nil
}
