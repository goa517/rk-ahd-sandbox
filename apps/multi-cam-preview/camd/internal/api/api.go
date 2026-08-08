// Package api 提供 HTTP 控制面：通道查询/参数调整 REST、统计 SSE、前端静态托管。
package api

import (
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"runtime"
	"sync"
	"time"

	"multi-cam-preview/camd/internal/config"
	"multi-cam-preview/camd/internal/pipeline"
	"multi-cam-preview/camd/internal/webrtcsrv"
	"multi-cam-preview/web"
)

// 编码器总吞吐估算预算（双核 rkvenc 约 4K@60 ≈ 500Mpx/s），用于负载估算展示
const encoderBudgetPxPerSec = 500_000_000.0

type Server struct {
	mgr *pipeline.Manager
	rtc *webrtcsrv.Server

	startedAt time.Time
	statsMu   sync.Mutex
	lastSample map[string]samplePoint

	platform string
	cpuModel string
	prevCPU  cpuTimes
}

type samplePoint struct {
	at     time.Time
	frames uint64
	bytes  uint64
}

func NewServer(mgr *pipeline.Manager, cfg *config.Config) (*Server, error) {
	rtc, err := webrtcsrv.NewServer(mgr)
	if err != nil {
		return nil, fmt.Errorf("初始化 WebRTC 失败: %w", err)
	}
	return &Server{
		mgr:        mgr,
		rtc:        rtc,
		startedAt:  time.Now(),
		lastSample: make(map[string]samplePoint),
		platform:   readPlatform(),
		cpuModel:   readCPUModel(),
		prevCPU:    readCPUTimes(),
	}, nil
}

func (s *Server) Router() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/channels", s.handleChannels)
	mux.HandleFunc("PUT /api/channels/{id}", s.handleUpdateChannel)
	mux.HandleFunc("GET /api/clock", s.handleClock)
	mux.HandleFunc("POST /api/whep/{id}", s.rtc.HandleOffer)
	mux.HandleFunc("POST /api/debug/rtc-stats", s.handleDebugRTCStats)
	mux.HandleFunc("GET /ws/stats", s.handleStatsSSE)
	mux.Handle("/", noCache(http.FileServerFS(web.FS())))
	return mux
}

// noCache 让静态资源每次请求都回源校验（配合 FileServer 的 304），避免浏览器缓存旧版前端。
func noCache(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-cache")
		h.ServeHTTP(w, r)
	})
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func (s *Server) handleChannels(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, s.mgr.Infos())
}

// handleDebugRTCStats 接收浏览器侧 WebRTC 统计（丢包/抖动/解码），写日志用于诊断。
func (s *Server) handleDebugRTCStats(w http.ResponseWriter, r *http.Request) {
	var v map[string]any
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<16)).Decode(&v); err != nil {
		http.Error(w, "非法请求体", http.StatusBadRequest)
		return
	}
	slog.Info("浏览器RTC统计", "ch", v["id"],
		"lost", v["lost"], "recv", v["recv"],
		"jitter_ms", v["jitter_ms"], "jb_ms", v["jb_ms"], "jb_target_ms", v["jb_target_ms"],
		"decode_ms", v["decode_ms"], "dropped", v["dropped"],
		"pli", v["pli"], "keyframes", v["keyframes"], "rtt_ms", v["rtt_ms"])
	w.WriteHeader(http.StatusNoContent)
}

// handleClock 返回板端当前墙上时钟（µs），供前端做时钟对时（计算采集延迟用）。
// 取时间紧邻写响应，最小化服务端内部耗时引入的误差。
func (s *Server) handleClock(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	_, _ = fmt.Fprintf(w, `{"server_us":%d}`, time.Now().UnixMicro())
}

func (s *Server) handleUpdateChannel(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	var p pipeline.ParamUpdate
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<16)).Decode(&p); err != nil {
		http.Error(w, "非法请求体: "+err.Error(), http.StatusBadRequest)
		return
	}
	info, err := s.mgr.Update(id, p)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	slog.Info("通道参数已更新", "id", id,
		"size", fmt.Sprintf("%dx%d@%d", info.Width, info.Height, info.FPS),
		"bitrate_kbps", info.BitrateKbps, "gop", info.GOP)
	writeJSON(w, http.StatusOK, info)
}

// ChannelStats 为单通道周期统计（fps/kbps 由计数器差分得出）。
type ChannelStats struct {
	ID      string  `json:"id"`
	Online  bool    `json:"online"`
	FPS     float64 `json:"fps"`
	Kbps    float64 `json:"kbps"`
	Viewers int     `json:"viewers"`
	Width   int     `json:"width"`
	Height  int     `json:"height"`
}

type Stats struct {
	Online         int            `json:"online"`
	Total          int            `json:"total"`
	EncoderLoadPct float64        `json:"encoder_load_pct"` // 基于编码预算的估算值
	UptimeSec      int64          `json:"uptime_sec"`
	Channels       []ChannelStats `json:"channels"`
	System         SysInfo        `json:"system"`
}

func (s *Server) collectStats() Stats {
	now := time.Now()
	infos := s.mgr.Infos()

	s.statsMu.Lock()
	defer s.statsMu.Unlock()

	st := Stats{Total: len(infos), UptimeSec: int64(now.Sub(s.startedAt).Seconds())}
	var pxPerSec float64
	for _, info := range infos {
		cs := ChannelStats{
			ID: info.ID, Online: info.Online, Viewers: info.Viewers,
			Width: info.Width, Height: info.Height,
		}
		if info.Online {
			st.Online++
			pxPerSec += float64(info.Width) * float64(info.Height) * float64(info.FPS)

			if ch := s.mgr.Get(info.ID); ch != nil {
				frames, bytes := ch.Stats()
				if prev, ok := s.lastSample[info.ID]; ok {
					dt := now.Sub(prev.at).Seconds()
					if dt > 0 {
						cs.FPS = float64(frames-prev.frames) / dt
						cs.Kbps = float64(bytes-prev.bytes) * 8 / 1000 / dt
					}
				}
				s.lastSample[info.ID] = samplePoint{at: now, frames: frames, bytes: bytes}
			}
		} else {
			delete(s.lastSample, info.ID)
		}
		st.Channels = append(st.Channels, cs)
	}
	st.EncoderLoadPct = pxPerSec / encoderBudgetPxPerSec * 100

	curCPU := readCPUTimes()
	usedMB, totalMB := readMemMB()
	st.System = SysInfo{
		Platform:   s.platform,
		CPU:        s.cpuModel,
		Cores:      runtime.NumCPU(),
		CPUPct:     cpuPercent(s.prevCPU, curCPU),
		MemUsedMB:  usedMB,
		MemTotalMB: totalMB,
		TempC:      readTempC(),
	}
	s.prevCPU = curCPU
	return st
}

func (s *Server) handleStatsSSE(w http.ResponseWriter, r *http.Request) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "不支持流式响应", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")

	ticker := time.NewTicker(2 * time.Second)
	defer ticker.Stop()

	push := func() bool {
		data, err := json.Marshal(s.collectStats())
		if err != nil {
			return true
		}
		if _, err := fmt.Fprintf(w, "data: %s\n\n", data); err != nil {
			return false
		}
		flusher.Flush()
		return true
	}

	if !push() {
		return
	}
	for {
		select {
		case <-r.Context().Done():
			return
		case <-ticker.C:
			if !push() {
				return
			}
		}
	}
}
