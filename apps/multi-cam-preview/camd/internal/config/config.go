package config

import (
	"fmt"
	"os"

	"gopkg.in/yaml.v3"
)

// Channel 为一路摄像头的声明式配置。
type Channel struct {
	ID          string `yaml:"id"`
	Name        string `yaml:"name"`
	Type        string `yaml:"type"` // raw / ahd / stitch
	Device      string `yaml:"device"`
	Enabled     bool   `yaml:"enabled"`
	Width       int    `yaml:"width"`
	Height      int    `yaml:"height"`
	FPS         int    `yaml:"fps"`
	BitrateKbps int    `yaml:"bitrate_kbps"`
	GOP         int    `yaml:"gop"`
	SourceFPS   int    `yaml:"source_fps"` // 采集源帧率（sensor/解码器输出）
	Format      string `yaml:"format"`     // NV12 / UYVY

	// 以下仅 type=stitch（环视拼接）使用：从 sources 通道 tap 原始帧，RGA 合成到画布
	Sources []string     `yaml:"sources"` // 源通道 id 列表
	Layout  []LayoutTile `yaml:"layout"`  // 每个源在画布上的放置（与 sources 一一对应）
}

// LayoutTile 描述一路源画面在拼接画布上的目标区域与变换。
type LayoutTile struct {
	X      int   `yaml:"x"`
	Y      int   `yaml:"y"`
	W      int   `yaml:"w"`
	H      int   `yaml:"h"`
	Rotate int   `yaml:"rotate"` // 顺时针 0/90/180/270
	Crop   []int `yaml:"crop"`   // 可选，源图裁剪 [x,y,w,h]，缺省整幅
}

type Config struct {
	Listen   string    `yaml:"listen"`
	Channels []Channel `yaml:"channels"`
}

func Load(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	cfg := &Config{}
	if err := yaml.Unmarshal(data, cfg); err != nil {
		return nil, fmt.Errorf("解析 %s 失败: %w", path, err)
	}
	if cfg.Listen == "" {
		cfg.Listen = ":8080"
	}
	for i := range cfg.Channels {
		c := &cfg.Channels[i]
		if c.SourceFPS <= 0 {
			c.SourceFPS = 30
		}
		if c.FPS <= 0 {
			c.FPS = c.SourceFPS
		}
		if c.FPS > c.SourceFPS {
			c.FPS = c.SourceFPS
		}
		if c.Width <= 0 {
			c.Width = 1280
		}
		if c.Height <= 0 {
			c.Height = 720
		}
		if c.BitrateKbps <= 0 {
			c.BitrateKbps = 2048
		}
		if c.GOP <= 0 {
			c.GOP = c.FPS * 2
		}
		if c.Format == "" {
			c.Format = "NV12"
		}
	}
	return cfg, nil
}
