package main

import (
	"context"
	"encoding/binary"
	"github.com/xjasonlyu/tun2socks/v2/core"
	"golang.org/x/sys/windows"
	"golang.zx2c4.com/wintun"
	"gvisor.dev/gvisor/pkg/buffer"
	"gvisor.dev/gvisor/pkg/tcpip/adapters/gonet"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/link/channel"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
	"gvisor.dev/gvisor/pkg/tcpip/transport/tcp"
	"gvisor.dev/gvisor/pkg/waiter"
	"log"
	"net/netip"
	"sync"
)

func runStack(ctx context.Context, c config, session wintun.Session, b *bridge) error {
	ctx, cancel := context.WithCancel(ctx)
	ep := channel.New(256, c.mtu, "")
	s, e := core.CreateStack(&core.Config{LinkEndpoint: ep, TransportHandler: b})
	if e != nil {
		cancel()
		return e
	}
	defer func() { s.Close(); s.Wait() }()
	defer ep.Close()
	// 真实连接到 B 的目标端口后才完成 A 的 TCP 握手，禁止产生虚假的端口可达结果。
	forwarder := tcp.NewForwarder(s, 0, 256, func(r *tcp.ForwarderRequest) {
		go func() {
			id := r.ID()
			remote, err := b.dial(1, id.LocalAddress.String(), id.LocalPort)
			if err != nil {
				r.Complete(true)
				log.Printf("内网 TCP 目标连接失败：%v", err)
				return
			}
			defer b.release(remote)
			var queue waiter.Queue
			endpoint, errTCP := r.CreateEndpoint(&queue)
			if errTCP != nil {
				r.Complete(true)
				return
			}
			r.Complete(false)
			local := gonet.NewTCPConn(&queue, endpoint)
			defer local.Close()
			pipe(local, remote)
		}()
	})
	s.SetTransportProtocolHandler(tcp.ProtocolNumber, forwarder.HandlePacket)
	var writeMu sync.Mutex
	write := func(packet []byte) error {
		writeMu.Lock()
		defer writeMu.Unlock()
		out, err := session.AllocateSendPacket(len(packet))
		if err != nil {
			return err
		}
		copy(out, packet)
		session.SendPacket(out)
		return nil
	}
	stopped := make(chan struct{})
	go func() {
		defer close(stopped)
		for {
			packet := ep.ReadContext(ctx)
			if packet == nil {
				return
			}
			view := packet.ToView()
			write(view.AsSlice())
			view.Release()
			packet.DecRef()
		}
	}()
	var echoes sync.WaitGroup
	defer func() { cancel(); b.close(); <-stopped; echoes.Wait() }()
	for ctx.Err() == nil {
		packet, err := session.ReceivePacket()
		if err == windows.ERROR_NO_MORE_ITEMS {
			windows.WaitForSingleObject(session.ReadWaitEvent(), 100)
			continue
		}
		if err != nil {
			return err
		}
		data := append([]byte(nil), packet...)
		session.ReleaseReceivePacket(packet)
		if len(data) < 20 || data[0]>>4 != 4 {
			continue
		}
		size := int(binary.BigEndian.Uint16(data[2:4]))
		if size < 20 || size > len(data) {
			continue
		}
		data = data[:size]
		ip := netip.AddrFrom4([4]byte(data[16:20]))
		if !internal(ip) {
			continue
		}
		if data[9] == 1 {
			echoes.Add(1)
			go func() { defer echoes.Done(); b.echo(data, write) }()
			continue
		}
		if data[9] != 6 && data[9] != 17 {
			continue
		}
		packetBuffer := stack.NewPacketBuffer(stack.PacketBufferOptions{Payload: buffer.MakeWithData(data)})
		ep.InjectInbound(header.IPv4ProtocolNumber, packetBuffer)
		packetBuffer.DecRef()
	}
	return nil
}
