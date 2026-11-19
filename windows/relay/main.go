package main

import (
	"fmt"
	"log"
	"net"
	"os"
	"path/filepath"
	"time"
)

type relayLog struct{ last string }

func (state *relayLog) changed(message string) {
	if state.last != message {
		log.Println(message)
		state.last = message
	}
}

func serveHost(c config) {
	var state relayLog
	for {
		fastRetry, err := hostConnection(c, &state)
		if err != nil {
			state.changed("被控端中转连接失败：" + err.Error())
		}
		if !fastRetry {
			time.Sleep(2 * time.Second)
		}
	}
}

func hostConnection(c config, state *relayLog) (bool, error) {
	socket, err := openWebSocket(c)
	if err != nil {
		return false, err
	}
	defer socket.Close()
	state.changed("已连上中转服务器，等待控制端")
	status, err := waitStatus(socket, 25*time.Second)
	if err != nil {
		return false, err
	}
	if status == waitExpired {
		return true, nil
	}
	if status != paired {
		return false, fmt.Errorf("设备已有中转连接，状态 %d", status)
	}
	address := c.local
	if c.consent != "" && consentReady(c.device) {
		address = c.consent
	}
	local, err := net.DialTimeout("tcp", address, 2*time.Second)
	if err != nil {
		return false, fmt.Errorf("本机被控端未监听：%w", err)
	}
	defer local.Close()
	state.changed("中转已配对，正在转发 WindowsStream 连接")
	if c.consent != "" {
		stop := watchConsent(c.device, address == c.consent, local)
		defer stop()
	}
	bridge(local, socket)
	return true, nil
}

func serveViewer(c config, announcePort bool) error {
	if announcePort {
		c.local = "127.0.0.1:0"
	}
	listener, err := net.Listen("tcp", c.local)
	if err != nil {
		return err
	}
	defer listener.Close()
	if announcePort {
		if _, err := fmt.Fprintln(os.Stdout, listener.Addr().(*net.TCPAddr).Port); err != nil {
			return fmt.Errorf("无法向控制端返回中转端口：%w", err)
		}
		_ = os.Stdout.Close()
	}
	log.Println("控制端中转已就绪")
	var state relayLog
	for {
		local, err := listener.Accept()
		if err != nil {
			return err
		}
		if err := viewerConnection(c, local, &state); err != nil {
			state.changed("控制端中转连接失败：" + err.Error())
		}
	}
}

func viewerConnection(c config, local net.Conn, state *relayLog) error {
	defer local.Close()
	socket, err := openWebSocket(c)
	if err != nil {
		return err
	}
	defer socket.Close()
	status, err := waitStatus(socket, 3*time.Second)
	if err != nil {
		return err
	}
	switch status {
	case paired:
		state.changed("中转已配对，正在转发 WindowsStream 连接")
		bridge(local, socket)
		return nil
	case offline:
		return fmt.Errorf("被控端尚未连上中转服务器")
	case busy:
		return fmt.Errorf("被控端已有中转会话")
	default:
		return fmt.Errorf("中转配对状态无效：%d", status)
	}
}

func main() {
	executable, err := os.Executable()
	if err != nil {
		log.Fatal("无法读取中转程序位置：", err)
	}
	root := filepath.Dir(executable)
	configPath := filepath.Join(root, "relay.ini")
	announcePort := len(os.Args) == 3 && os.Args[1] == "--viewer-config"
	if announcePort {
		configPath = os.Args[2]
		root = filepath.Dir(configPath)
	} else if len(os.Args) != 1 {
		log.Fatal("中转组件启动参数无效")
	}
	file, err := os.OpenFile(filepath.Join(root, "relay.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		log.Fatal("无法打开中转日志：", err)
	}
	defer file.Close()
	log.SetOutput(file)
	c, err := loadConfig(configPath)
	if err != nil {
		log.Fatal("读取中转配置失败：", err)
	}
	if c.role == "host" {
		if announcePort {
			log.Fatal("控制端需要使用查看端中转配置")
		}
		serveHost(c)
		return
	}
	if err := serveViewer(c, announcePort); err != nil {
		log.Fatal("控制端中转已退出：", err)
	}
}
