package main

import (
	"encoding/binary"
	"fmt"
	"github.com/xjasonlyu/tun2socks/v2/core/adapter"
	"io"
	"log"
	"net"
	"net/netip"
	"sync"
	"time"
)

type bridge struct {
	endpoint string
	mu       sync.Mutex
	peers    map[net.Conn]bool
	closed   bool
	limit    chan struct{}
}

func (b *bridge) dial(kind byte, host string, port uint16) (net.Conn, error) {
	if kind != 4 {
		ip, e := netip.ParseAddr(host)
		if e != nil || !internal(ip) {
			return nil, fmt.Errorf("目标不在允许的内网网段")
		}
	}
	b.mu.Lock()
	closed := b.closed
	b.mu.Unlock()
	if closed {
		return nil, net.ErrClosed
	}
	c, e := net.DialTimeout("tcp", b.endpoint, 3*time.Second)
	if e != nil {
		return nil, e
	}
	b.mu.Lock()
	if b.closed {
		b.mu.Unlock()
		c.Close()
		return nil, net.ErrClosed
	}
	b.peers[c] = true
	b.mu.Unlock()
	c.SetDeadline(time.Now().Add(12 * time.Second))
	header := []byte{127, kind}
	if kind != 4 {
		header = binary.LittleEndian.AppendUint16(header, port)
		header = binary.LittleEndian.AppendUint16(header, uint16(len(host)))
		header = append(header, host...)
	}
	if e = send(c, header); e == nil {
		var status [1]byte
		_, e = io.ReadFull(c, status[:])
		if e == nil && status[0] != 0 {
			e = fmt.Errorf("被控端连接失败，状态 %d", status[0])
		}
	}
	if e != nil {
		b.release(c)
		return nil, e
	}
	c.SetDeadline(time.Time{})
	return c, nil
}
func (b *bridge) release(c net.Conn) { c.Close(); b.mu.Lock(); delete(b.peers, c); b.mu.Unlock() }
func (b *bridge) close() {
	b.mu.Lock()
	b.closed = true
	for c := range b.peers {
		c.Close()
	}
	b.mu.Unlock()
}
func send(c net.Conn, p []byte) error {
	for len(p) > 0 {
		n, e := c.Write(p)
		if e != nil {
			return e
		}
		if n == 0 {
			return io.ErrShortWrite
		}
		p = p[n:]
	}
	return nil
}
func (b *bridge) capabilities() error {
	c, e := b.dial(4, "", 0)
	if e != nil {
		return e
	}
	defer b.release(c)
	c.SetReadDeadline(time.Now().Add(5 * time.Second))
	var raw [4]byte
	if _, e = io.ReadFull(c, raw[:]); e != nil {
		return fmt.Errorf("被控端版本不支持 IP 隧道：%w", e)
	}
	if binary.LittleEndian.Uint32(raw[:]) != 0x00010007 {
		return fmt.Errorf("被控端尚未启用 TCP、UDP 和真实回显能力")
	}
	return nil
}
func (b *bridge) HandleTCP(local adapter.TCPConn) {
	go func() {
		defer local.Close()
		id := local.ID()
		remote, e := b.dial(1, id.LocalAddress.String(), id.LocalPort)
		if e != nil {
			log.Printf("内网 TCP 连接失败：%v", e)
			return
		}
		defer b.release(remote)
		pipe(local, remote)
	}()
}
func pipe(local, remote net.Conn) {
	done := make(chan struct{})
	go func() {
		io.Copy(remote, local)
		if c, ok := remote.(*net.TCPConn); ok {
			c.CloseWrite()
		}
		close(done)
	}()
	io.Copy(local, remote)
	if c, ok := local.(interface{ CloseWrite() error }); ok {
		c.CloseWrite()
	}
	local.SetReadDeadline(time.Now())
	<-done
}
func (b *bridge) HandleUDP(local adapter.UDPConn) {
	go func() {
		defer local.Close()
		id := local.ID()
		remote, e := b.dial(2, id.LocalAddress.String(), id.LocalPort)
		if e != nil {
			log.Printf("内网 UDP 连接失败：%v", e)
			return
		}
		defer b.release(remote)
		done := make(chan struct{})
		go func() {
			defer close(done)
			buf := make([]byte, 65507)
			for {
				local.SetReadDeadline(time.Now().Add(time.Minute))
				n, _, e := local.ReadFrom(buf)
				if e != nil {
					return
				}
				frame := binary.LittleEndian.AppendUint16(nil, uint16(n))
				frame = append(frame, buf[:n]...)
				remote.SetWriteDeadline(time.Now().Add(5 * time.Second))
				if send(remote, frame) != nil {
					return
				}
			}
		}()
		for {
			var raw [2]byte
			remote.SetReadDeadline(time.Now().Add(65 * time.Second))
			if _, e = io.ReadFull(remote, raw[:]); e != nil {
				break
			}
			n := int(binary.LittleEndian.Uint16(raw[:]))
			if n > 65507 {
				break
			}
			buf := make([]byte, n)
			if _, e = io.ReadFull(remote, buf); e != nil {
				break
			}
			if _, e = local.Write(buf); e != nil {
				break
			}
		}
		local.Close()
		remote.Close()
		<-done
	}()
}
