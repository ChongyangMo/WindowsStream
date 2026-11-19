package main

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"net"
	"net/url"
	"os"
	"strconv"
	"strings"
)

type config struct {
	role       string
	endpoint   string
	server     string
	httpProxy  string
	device     string
	token      string
	local      string
	enrollment string
	viewerHash string
	consent    string
}

func loadConfig(path string) (config, error) {
	var c config
	file, err := os.Open(path)
	if err != nil {
		return c, err
	}
	defer file.Close()

	values := map[string]*string{
		"role": &c.role, "endpoint": &c.endpoint, "server_address": &c.server,
		"http_proxy": &c.httpProxy, "device": &c.device, "token": &c.token,
		"local_address": &c.local,
		"enrollment":    &c.enrollment, "viewer_sha256": &c.viewerHash,
		"consent_address": &c.consent,
	}
	scanner := bufio.NewScanner(file)
	for lineNumber := 1; scanner.Scan(); lineNumber++ {
		line := strings.TrimSpace(strings.TrimPrefix(scanner.Text(), "\ufeff"))
		if line == "" || line == "[relay]" || strings.HasPrefix(line, ";") {
			continue
		}
		key, value, ok := strings.Cut(line, "=")
		field := values[strings.TrimSpace(key)]
		if !ok || field == nil {
			return c, fmt.Errorf("中转配置第 %d 行无效", lineNumber)
		}
		*field = strings.TrimSpace(value)
	}
	if err := scanner.Err(); err != nil {
		return c, err
	}
	if c.role != "host" && c.role != "viewer" {
		return c, fmt.Errorf("中转角色应为 host 或 viewer")
	}
	endpoint, err := url.Parse(c.endpoint)
	if err != nil || endpoint.Scheme != "wss" || endpoint.Host == "" || endpoint.User != nil ||
		endpoint.RawQuery != "" || endpoint.Fragment != "" || endpoint.Path != "/api/windowsstream/relay" {
		return c, fmt.Errorf("中转入口应为 WindowsStream 的 WSS 地址")
	}
	if c.server == "" {
		port := endpoint.Port()
		if port == "" {
			port = "443"
		}
		c.server = net.JoinHostPort(endpoint.Hostname(), port)
	}
	addresses := []string{c.local, c.server}
	if c.httpProxy != "" {
		addresses = append(addresses, c.httpProxy)
	}
	for _, address := range addresses {
		host, port, err := net.SplitHostPort(address)
		number, invalid := strconv.ParseUint(port, 10, 16)
		if err != nil || invalid != nil || host == "" || number == 0 {
			return c, fmt.Errorf("中转地址或端口无效")
		}
	}
	if !strings.HasPrefix(c.local, "127.0.0.1:") {
		return c, fmt.Errorf("本地中转地址只能使用回环地址")
	}
	if c.consent != "" {
		host, port, err := net.SplitHostPort(c.consent)
		number, invalid := strconv.ParseUint(port, 10, 16)
		if c.role != "host" || host != "127.0.0.1" || err != nil || invalid != nil ||
			number == 0 || c.consent == c.local {
			return c, fmt.Errorf("UAC 中转地址无效")
		}
	}
	key, err := hex.DecodeString(c.token)
	if err != nil || len(key) != 32 {
		return c, fmt.Errorf("中转口令无效")
	}
	if c.device == "" || len(c.device) > 64 || strings.Trim(c.device, "abcdefghijklmnopqrstuvwxyz0123456789-") != "" {
		return c, fmt.Errorf("设备标识无效")
	}
	if c.enrollment != "" || c.viewerHash != "" {
		enrollment, enrollErr := hex.DecodeString(c.enrollment)
		viewerHash, viewerErr := hex.DecodeString(c.viewerHash)
		if c.role != "host" || len(enrollment) != 32 || len(viewerHash) != 32 || enrollErr != nil || viewerErr != nil {
			return c, fmt.Errorf("被控端登记信息无效")
		}
	}
	return c, nil
}
