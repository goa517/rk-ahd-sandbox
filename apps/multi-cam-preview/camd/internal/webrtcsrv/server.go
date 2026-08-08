// Package webrtcsrv 提供每通道一条 PeerConnection 的 WHEP 风格 SDP 交换与发流。
package webrtcsrv

import (
	"io"
	"log/slog"
	"math/rand/v2"
	"net/http"
	"sync"

	"github.com/pion/rtcp"
	"github.com/pion/rtp"
	"github.com/pion/rtp/codecs"
	"github.com/pion/webrtc/v4"

	"multi-cam-preview/camd/internal/pipeline"
)

const (
	mimeTypeH265  = "video/H265"
	rtpMTU        = 1200
	rtpPayloadTyp = 96
	rtpClockRate  = 90000
)

type Server struct {
	api *webrtc.API
	mgr *pipeline.Manager
}

func NewServer(mgr *pipeline.Manager) (*Server, error) {
	me := &webrtc.MediaEngine{}
	if err := me.RegisterCodec(webrtc.RTPCodecParameters{
		RTPCodecCapability: webrtc.RTPCodecCapability{
			MimeType:  mimeTypeH265,
			ClockRate: 90000,
		},
		PayloadType: 96,
	}, webrtc.RTPCodecTypeVideo); err != nil {
		return nil, err
	}
	se := webrtc.SettingEngine{}
	se.SetLite(true) // 服务端 ICE-lite，上位机为 full ICE，LAN 内免 STUN
	api := webrtc.NewAPI(webrtc.WithMediaEngine(me), webrtc.WithSettingEngine(se))
	return &Server{api: api, mgr: mgr}, nil
}

// HandleOffer 处理 POST /api/whep/{id}：收 SDP offer，回 SDP answer（非 trickle）。
func (s *Server) HandleOffer(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	ch := s.mgr.Get(id)
	if ch == nil || !ch.Online() {
		http.Error(w, "channel offline", http.StatusNotFound)
		return
	}
	offerSDP, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 1<<20))
	if err != nil {
		http.Error(w, "读取 offer 失败", http.StatusBadRequest)
		return
	}

	pc, err := s.api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	// 手工 packetize（RTP 时间戳写入帧采集时刻），故用 StaticRTP 而非 StaticSample
	track, err := webrtc.NewTrackLocalStaticRTP(
		webrtc.RTPCodecCapability{MimeType: mimeTypeH265, ClockRate: rtpClockRate},
		"video", "camd-"+id,
	)
	if err != nil {
		_ = pc.Close()
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	sender, err := pc.AddTrack(track)
	if err != nil {
		_ = pc.Close()
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	// RTCP 回包：PLI/FIR → 请求编码器出 IDR
	go func() {
		buf := make([]byte, 1500)
		for {
			n, _, err := sender.Read(buf)
			if err != nil {
				return
			}
			pkts, err := rtcp.Unmarshal(buf[:n])
			if err != nil {
				continue
			}
			for _, p := range pkts {
				switch p.(type) {
				case *rtcp.PictureLossIndication, *rtcp.FullIntraRequest:
					ch.RequestIDR()
				}
			}
		}
	}()

	done := make(chan struct{})
	var closeOnce sync.Once
	pc.OnConnectionStateChange(func(st webrtc.PeerConnectionState) {
		switch st {
		case webrtc.PeerConnectionStateFailed,
			webrtc.PeerConnectionStateClosed,
			webrtc.PeerConnectionStateDisconnected:
			closeOnce.Do(func() {
				close(done)
				_ = pc.Close()
			})
		}
	})

	if err := pc.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer, SDP: string(offerSDP),
	}); err != nil {
		_ = pc.Close()
		http.Error(w, "非法 offer: "+err.Error(), http.StatusBadRequest)
		return
	}
	answer, err := pc.CreateAnswer(nil)
	if err != nil {
		_ = pc.Close()
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if err := pc.SetLocalDescription(answer); err != nil {
		_ = pc.Close()
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	<-webrtc.GatheringCompletePromise(pc)

	w.Header().Set("Content-Type", "application/sdp")
	_, _ = w.Write([]byte(pc.LocalDescription().SDP))

	slog.Info("观众接入", "channel", id)
	go s.pump(track, ch, done)
}

// pump 将通道编码帧写入该观众的 track。等待首个 IDR 后起播，避免花屏。
// 每个 AU 的所有 RTP 分片时间戳统一覆写为采集时刻（90kHz 墙上时钟），
// 供浏览器经 getSynchronizationSources 还原采集时间/计算端到端延迟。
func (s *Server) pump(track *webrtc.TrackLocalStaticRTP, ch *pipeline.Channel, done chan struct{}) {
	sub, _, _ := ch.Hub().Subscribe()
	defer func() {
		ch.Hub().Unsubscribe(sub)
		slog.Info("观众离开", "channel", ch.ID())
	}()

	pk := rtp.NewPacketizer(
		rtpMTU, rtpPayloadTyp, rand.Uint32(),
		&codecs.H265Payloader{},
		rtp.NewRandomSequencer(),
		rtpClockRate,
	)
	samples := uint32(ch.FrameDuration().Microseconds() * rtpClockRate / 1e6)

	ch.RequestIDR()
	started := false
	for {
		select {
		case <-done:
			return
		case f := <-sub.C:
			if !started {
				if !f.Keyframe {
					continue
				}
				started = true
			}
			pkts := pk.Packetize(f.Data, samples)
			// 采集时刻 → 90kHz RTP 时间戳。用 ×9/100 避免 int64 溢出
			//（epoch µs × 90000 ≈ 1.6e20 > 2^63）
			ts := uint32(f.CaptureWallUs * 9 / 100)
			for _, p := range pkts {
				p.Header.Timestamp = ts
				if err := track.WriteRTP(p); err != nil {
					return
				}
			}
		}
	}
}
