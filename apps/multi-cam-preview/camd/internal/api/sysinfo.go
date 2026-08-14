package api

import (
	"os"
	"runtime"
	"strconv"
	"strings"
)

// SysInfo 为板端系统信息（随 SSE 统计推送）。
type SysInfo struct {
	Platform   string  `json:"platform"`     // 设备树 model（板型）
	CPU        string  `json:"cpu"`          // 处理器型号
	Cores      int     `json:"cores"`        // 核数
	CPUPct     float64 `json:"cpu_pct"`      // 全核平均占用 %
	MemUsedMB  int     `json:"mem_used_mb"`  // 已用内存（Total-Available）
	MemTotalMB int     `json:"mem_total_mb"` // 总内存
	TempC      float64 `json:"temp_c"`       // SoC 温度
	NetRxKbps  float64 `json:"net_rx_kbps"`  // 网络接收吞吐（除 lo 外全部接口合计）
	NetTxKbps  float64 `json:"net_tx_kbps"`  // 网络发送吞吐
}

func readPlatform() string {
	if b, err := os.ReadFile("/proc/device-tree/model"); err == nil {
		if s := strings.TrimRight(string(b), "\x00\n "); s != "" {
			return s
		}
	}
	return runtime.GOOS + "/" + runtime.GOARCH
}

// readCPUModel 优先从设备树 compatible 提取 SoC 型号（如 rockchip,rk3576 → RK3576），
// 否则回退 /proc/cpuinfo 的 model name（x86）。
func readCPUModel() string {
	if b, err := os.ReadFile("/proc/device-tree/compatible"); err == nil {
		for _, c := range strings.Split(string(b), "\x00") {
			if strings.HasPrefix(c, "rockchip,") {
				return strings.ToUpper(strings.TrimPrefix(c, "rockchip,"))
			}
		}
	}
	if b, err := os.ReadFile("/proc/cpuinfo"); err == nil {
		for _, l := range strings.Split(string(b), "\n") {
			if strings.HasPrefix(l, "model name") {
				if i := strings.Index(l, ":"); i > 0 {
					return strings.TrimSpace(l[i+1:])
				}
			}
		}
	}
	return "unknown"
}

// readTempC 读取 SoC 温度（优先 type 含 soc/cpu 的 thermal zone，否则 zone0）。
func readTempC() float64 {
	zones, err := os.ReadDir("/sys/class/thermal")
	if err != nil {
		return 0
	}
	fallback := -1.0
	for _, z := range zones {
		base := "/sys/class/thermal/" + z.Name()
		tb, _ := os.ReadFile(base + "/type")
		typ := strings.TrimSpace(string(tb))
		vb, err := os.ReadFile(base + "/temp")
		if err != nil {
			continue
		}
		mv, err := strconv.Atoi(strings.TrimSpace(string(vb)))
		if err != nil {
			continue
		}
		if strings.Contains(typ, "soc") || strings.Contains(typ, "cpu") || strings.Contains(typ, "package") {
			return float64(mv) / 1000
		}
		if fallback < 0 {
			fallback = float64(mv) / 1000
		}
	}
	if fallback > 0 {
		return fallback
	}
	return 0
}

func readMemMB() (used, total int) {
	b, err := os.ReadFile("/proc/meminfo")
	if err != nil {
		return 0, 0
	}
	var tot, avail int
	for _, l := range strings.Split(string(b), "\n") {
		f := strings.Fields(l)
		if len(f) < 2 {
			continue
		}
		v, _ := strconv.Atoi(f[1])
		switch f[0] {
		case "MemTotal:":
			tot = v
		case "MemAvailable:":
			avail = v
		}
	}
	return (tot - avail) / 1024, tot / 1024
}

type cpuTimes struct{ idle, total uint64 }

func readCPUTimes() cpuTimes {
	b, err := os.ReadFile("/proc/stat")
	if err != nil {
		return cpuTimes{}
	}
	for _, l := range strings.Split(string(b), "\n") {
		if !strings.HasPrefix(l, "cpu ") {
			continue
		}
		f := strings.Fields(l)
		var t cpuTimes
		for i, s := range f[1:] {
			v, _ := strconv.ParseUint(s, 10, 64)
			t.total += v
			if i == 3 || i == 4 { // idle + iowait
				t.idle += v
			}
		}
		return t
	}
	return cpuTimes{}
}

// cpuPercent 按两次采样差分计算全核平均占用。
func cpuPercent(prev, cur cpuTimes) float64 {
	dt := cur.total - prev.total
	di := cur.idle - prev.idle
	if dt == 0 || di > dt {
		return 0
	}
	return float64(dt-di) / float64(dt) * 100
}

type netBytes struct{ rx, tx uint64 }

// readNetBytes 汇总除 lo 外全部接口的累计收/发字节数（/proc/net/dev）。
func readNetBytes() netBytes {
	b, err := os.ReadFile("/proc/net/dev")
	if err != nil {
		return netBytes{}
	}
	var n netBytes
	for _, l := range strings.Split(string(b), "\n") {
		i := strings.Index(l, ":")
		if i < 0 {
			continue
		}
		iface := strings.TrimSpace(l[:i])
		if iface == "" || iface == "lo" {
			continue
		}
		f := strings.Fields(l[i+1:])
		if len(f) < 9 {
			continue
		}
		rx, _ := strconv.ParseUint(f[0], 10, 64)
		tx, _ := strconv.ParseUint(f[8], 10, 64)
		n.rx += rx
		n.tx += tx
	}
	return n
}
