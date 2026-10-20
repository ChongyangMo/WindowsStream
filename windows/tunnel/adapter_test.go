package main

import (
	"flag"
	"net"
	"net/netip"
	"testing"
	"time"
)

var testRealAdapter = flag.Bool("real-adapter", false, "验证真实网卡")
var testHoldAdapter = flag.Bool("hold-adapter", false, "验证异常退出清理")

// 验证真实驱动与真实路由的创建、清理；不发送模拟业务数据。
func TestRealAdapterLifecycle(t *testing.T) {
	if !*testRealAdapter {
		t.Skip("未要求真实网卡检查")
	}
	c := config{name: "WindowsStream", address: netip.MustParsePrefix("172.31.255.1/30"), mtu: 1280}
	v, e := openAdapter(c)
	if e != nil {
		t.Fatal(e)
	}
	if _, e = net.InterfaceByName(c.name); e != nil {
		v.Close()
		t.Fatal(e)
	}
	if _, e = v.luid.IPAddress(c.address.Addr()); e != nil {
		v.Close()
		t.Fatal(e)
	}
	if _, e = v.luid.Route(targetPrefix, netip.IPv4Unspecified()); e != nil {
		v.Close()
		t.Fatal(e)
	}
	v.Close()
	for i := 0; i < 30; i++ {
		if _, e = net.InterfaceByName(c.name); e != nil {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatal("虚拟网卡未在关闭后移除")
}

// 仅供验证进程异常结束后的驱动清理，需显式选中该测试。
func TestHoldRealAdapter(t *testing.T) {
	if !*testHoldAdapter {
		t.Skip("需要显式详细测试模式")
	}
	v, e := openAdapter(config{name: "WindowsStream", address: netip.MustParsePrefix("172.31.255.1/30"), mtu: 1280})
	if e != nil {
		t.Fatal(e)
	}
	defer v.Close()
	t.Log("真实网卡就绪，等待异常退出检查")
	time.Sleep(time.Minute)
}
