package main

import (
	"fmt"
	"golang.org/x/sys/windows"
	"net"
	"net/netip"
	"path/filepath"
	"strconv"
	"syscall"
	"unsafe"
)

var profileRead = windows.NewLazySystemDLL("kernel32.dll").NewProc("GetPrivateProfileStringW")
var profileWrite = windows.NewLazySystemDLL("kernel32.dll").NewProc("WritePrivateProfileStringW")

type config struct {
	path, state, endpoint, name string
	address                     netip.Prefix
	mtu                         uint32
}

func ini(path, section, key, fallback string) string {
	b := make([]uint16, 4096)
	p, _ := windows.UTF16PtrFromString(path)
	s, _ := windows.UTF16PtrFromString(section)
	k, _ := windows.UTF16PtrFromString(key)
	f, _ := windows.UTF16PtrFromString(fallback)
	n, _, _ := profileRead.Call(uintptr(unsafe.Pointer(s)), uintptr(unsafe.Pointer(k)), uintptr(unsafe.Pointer(f)), uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)), uintptr(unsafe.Pointer(p)))
	if n >= uintptr(len(b)-1) {
		return ""
	}
	return windows.UTF16ToString(b[:n])
}
func state(path, status, message string) {
	for key, value := range map[string]string{"status": status, "message": message, "process": strconv.Itoa(syscall.Getpid())} {
		p, _ := windows.UTF16PtrFromString(path)
		s, _ := windows.UTF16PtrFromString("tunnel")
		k, _ := windows.UTF16PtrFromString(key)
		v, _ := windows.UTF16PtrFromString(value)
		profileWrite.Call(uintptr(unsafe.Pointer(s)), uintptr(unsafe.Pointer(k)), uintptr(unsafe.Pointer(v)), uintptr(unsafe.Pointer(p)))
	}
}
func load(path string) (config, error) {
	c := config{path: path, state: filepath.Join(filepath.Dir(path), ".windowsstream-tunnel.ini"), name: "WindowsStream"}
	port, err := strconv.Atoi(ini(path, "proxy", "local_port", "1080"))
	if err != nil || port < 1 || port > 65535 {
		return c, fmt.Errorf("本机转发端口无效")
	}
	c.endpoint = net.JoinHostPort("127.0.0.1", strconv.Itoa(port))
	c.address, err = netip.ParsePrefix(ini(path, "intranet", "tun_address", "172.31.255.1/30"))
	if err != nil || !c.address.Addr().Is4() || c.address.Bits() != 30 || internal(c.address.Addr()) {
		return c, fmt.Errorf("虚拟网卡地址必须为内网目标以外的 IPv4 /30 地址")
	}
	n, err := strconv.Atoi(ini(path, "intranet", "tun_mtu", "1280"))
	if err != nil || n < 576 || n > 1500 {
		return c, fmt.Errorf("虚拟网卡 MTU 必须为 576 到 1500")
	}
	c.mtu = uint32(n)
	// 本版只把用户指定的 10.18 网段导入 IP 隧道，不创建默认路由。
	if ini(path, "intranet", "networks", "10.18.0.0/16") != "10.18.0.0/16" {
		return c, fmt.Errorf("当前 IP 隧道仅支持 10.18.0.0/16，其他规则仍可使用网页分流")
	}
	return c, nil
}
func internal(ip netip.Addr) bool { return netip.MustParsePrefix("10.18.0.0/16").Contains(ip) }
