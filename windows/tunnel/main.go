package main

import (
	"context"
	"flag"
	"fmt"
	"golang.org/x/sys/windows"
	"log"
	"net"
	"os"
	"path/filepath"
	"time"
)

func creation(handle windows.Handle) (uint64, error) {
	var a, b, c, d windows.Filetime
	e := windows.GetProcessTimes(handle, &a, &b, &c, &d)
	return uint64(a.HighDateTime)<<32 | uint64(a.LowDateTime), e
}
func run() (result error) {
	exe, e := os.Executable()
	if e != nil {
		return e
	}
	cfgPath := flag.String("config", filepath.Join(filepath.Dir(exe), "client.ini"), "")
	parentID := flag.Uint("parent", 0, "")
	created := flag.Uint64("created", 0, "")
	stopName := flag.String("stop", "", "")
	flag.Parse()
	logFile, e := os.OpenFile(filepath.Join(filepath.Dir(exe), "tunnel.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if e != nil {
		return e
	}
	defer logFile.Close()
	log.SetOutput(logFile)
	mutexName, _ := windows.UTF16PtrFromString("Local\\WindowsStreamIpTunnel")
	mutex, mutexErr := windows.CreateMutex(nil, true, mutexName)
	if mutexErr != nil {
		if mutex != 0 {
			windows.CloseHandle(mutex)
		}
		return fmt.Errorf("另一个 IP 隧道实例正在运行：%w", mutexErr)
	}
	defer windows.CloseHandle(mutex)
	c, e := load(*cfgPath)
	defer func() {
		if result != nil {
			if c.state != "" {
				state(c.state, "error", result.Error())
			}
			log.Printf("IP 隧道失败：%v", result)
		}
	}()
	if e != nil {
		return e
	}
	state(c.state, "starting", "正在检查被控端与虚拟网卡")
	var parent, stop windows.Handle
	if *parentID == 0 || *created == 0 || *stopName == "" {
		return fmt.Errorf("IP 隧道必须由控制端启动")
	}
	parent, e = windows.OpenProcess(windows.SYNCHRONIZE|windows.PROCESS_QUERY_LIMITED_INFORMATION, false, uint32(*parentID))
	if e != nil {
		return e
	}
	defer windows.CloseHandle(parent)
	actual, err := creation(parent)
	if err != nil || actual != *created {
		return fmt.Errorf("控制端进程身份不匹配")
	}
	name, e := windows.UTF16PtrFromString(*stopName)
	if e != nil {
		return e
	}
	stop, e = windows.OpenEvent(windows.SYNCHRONIZE, false, name)
	if e != nil {
		return e
	}
	defer windows.CloseHandle(stop)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go func() {
		windows.WaitForMultipleObjects([]windows.Handle{parent, stop}, false, windows.INFINITE)
		cancel()
	}()
	b := &bridge{endpoint: c.endpoint, peers: map[net.Conn]bool{}, limit: make(chan struct{}, 16)}
	defer b.close()
	go func() { <-ctx.Done(); b.close() }()
	if e = b.capabilities(); e != nil {
		state(c.state, "error", e.Error())
		return e
	}
	v, err := openAdapter(c)
	if err != nil {
		e = err
		state(c.state, "error", e.Error())
		return e
	}
	defer v.Close()
	state(c.state, "ready", "TCP、UDP 和真实回显已就绪")
	e = runStack(ctx, c, v.session, b)
	if ctx.Err() != nil {
		state(c.state, "stopped", "IP 隧道已停止")
		return nil
	}
	return e
}
func main() {
	if e := run(); e != nil {
		time.Sleep(100 * time.Millisecond)
		os.Exit(1)
	}
}
