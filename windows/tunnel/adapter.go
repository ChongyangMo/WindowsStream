package main

import (
	"fmt"
	"golang.org/x/sys/windows"
	"golang.zx2c4.com/wintun"
	"golang.zx2c4.com/wireguard/windows/tunnel/winipcfg"
	"log"
	"net"
	"net/netip"
)

var targetPrefix = netip.MustParsePrefix("10.18.0.0/16")

type virtualAdapter struct {
	adapter *wintun.Adapter
	session wintun.Session
	luid    winipcfg.LUID
	address netip.Prefix
	routed  bool
	started bool
}

func openAdapter(c config) (*virtualAdapter, error) {
	// 只创建本程序专用网卡；同名网卡存在时拒绝接管，避免改动其他 VPN。
	if old, e := net.InterfaceByName(c.name); e == nil && old != nil {
		return nil, fmt.Errorf("同名虚拟网卡已存在，请先关闭另一实例")
	}
	interfaces, e := net.Interfaces()
	if e != nil {
		return nil, e
	}
	for _, it := range interfaces {
		addresses, _ := it.Addrs()
		for _, a := range addresses {
			p, e := netip.ParsePrefix(a.String())
			if e == nil && p.Addr().Is4() && (p.Contains(c.address.Addr()) || c.address.Contains(p.Addr())) {
				return nil, fmt.Errorf("虚拟网卡地址与现有接口冲突：%s", it.Name)
			}
		}
	}
	guid, e := windows.GUIDFromString("{8c8f52a7-2e3a-456e-9a26-3f49c4d570bc}")
	if e != nil {
		return nil, e
	}
	adapter, e := wintun.CreateAdapter(c.name, "WindowsStream", &guid)
	if e != nil {
		return nil, fmt.Errorf("创建虚拟网卡失败，需要管理员权限：%w", e)
	}
	v := &virtualAdapter{adapter: adapter, luid: winipcfg.LUID(adapter.LUID()), address: c.address}
	ok := false
	defer func() {
		if !ok {
			v.Close()
		}
	}()
	row, e := v.luid.IPInterface(windows.AF_INET)
	if e != nil {
		return nil, e
	}
	row.NLMTU = c.mtu
	row.UseAutomaticMetric = false
	row.Metric = 1
	row.DadTransmits = 0
	row.RouterDiscoveryBehavior = winipcfg.RouterDiscoveryDisabled
	if e = row.Set(); e != nil {
		return nil, e
	}
	if e = v.luid.AddIPAddress(c.address); e != nil {
		return nil, e
	}
	v.session, e = adapter.StartSession(4 << 20)
	if e != nil {
		return nil, e
	}
	v.started = true
	if e = v.luid.AddRoute(targetPrefix, netip.IPv4Unspecified(), 0); e != nil {
		return nil, e
	}
	v.routed = true
	log.Printf("IP 隧道路由已启用：%s，经虚拟网卡 %s，MTU %d", targetPrefix, c.name, c.mtu)
	ok = true
	return v, nil
}
func (v *virtualAdapter) Close() {
	if v.routed {
		if e := v.luid.DeleteRoute(targetPrefix, netip.IPv4Unspecified()); e != nil {
			log.Printf("删除专用内网路由失败：%v", e)
		}
		v.routed = false
	}
	if e := v.luid.DeleteIPAddress(v.address); e != nil {
		log.Printf("清理虚拟网卡地址：%v", e)
	}
	if v.started {
		v.session.End()
		v.started = false
	}
	v.adapter.Close()
	log.Print("专用内网路由和虚拟网卡已清理")
}
