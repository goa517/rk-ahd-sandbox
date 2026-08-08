// Package web 内嵌纯静态前端资源，由 daemon 直接托管。
package web

import (
	"embed"
	"io/fs"
)

//go:embed index.html app.js style.css
var content embed.FS

// FS 返回前端静态文件系统。
func FS() fs.FS { return content }
