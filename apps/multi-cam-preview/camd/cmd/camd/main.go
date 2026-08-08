package main

import (
	"context"
	"flag"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"multi-cam-preview/camd/internal/api"
	"multi-cam-preview/camd/internal/config"
	"multi-cam-preview/camd/internal/pipeline"
)

func main() {
	cfgPath := flag.String("config", "config.yaml", "配置文件路径")
	addr := flag.String("addr", "", "覆盖监听地址")
	flag.Parse()

	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo})))

	cfg, err := config.Load(*cfgPath)
	if err != nil {
		slog.Error("加载配置失败", "err", err)
		os.Exit(1)
	}
	if *addr != "" {
		cfg.Listen = *addr
	}

	mgr := pipeline.NewManager(cfg)
	mgr.Start()

	srv, err := api.NewServer(mgr, cfg)
	if err != nil {
		slog.Error("初始化服务失败", "err", err)
		os.Exit(1)
	}
	httpSrv := &http.Server{Addr: cfg.Listen, Handler: srv.Router()}

	go func() {
		slog.Info("HTTP 服务启动", "addr", cfg.Listen)
		if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			slog.Error("HTTP 服务异常退出", "err", err)
			os.Exit(1)
		}
	}()

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig

	slog.Info("正在退出")
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = httpSrv.Shutdown(ctx)
	mgr.Stop()
}
