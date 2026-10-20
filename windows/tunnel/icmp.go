package main

import (
	"encoding/binary"
	"io"
	"log"
	"net/netip"
	"time"
)

func checksum(data []byte) uint16 {
	var sum uint32
	for len(data) > 1 {
		sum += uint32(binary.BigEndian.Uint16(data))
		data = data[2:]
	}
	if len(data) > 0 {
		sum += uint32(data[0]) << 8
	}
	for sum>>16 != 0 {
		sum = (sum & 65535) + (sum >> 16)
	}
	return ^uint16(sum)
}
func (b *bridge) echo(packet []byte, write func([]byte) error) {
	select {
	case b.limit <- struct{}{}:
	default:
		return
	}
	defer func() { <-b.limit }()
	h := int(packet[0]&15) * 4
	if h < 20 || len(packet) < h+8 || packet[h] != 8 || packet[h+1] != 0 || binary.BigEndian.Uint16(packet[6:8])&0x3fff != 0 {
		return
	}
	target := netip.AddrFrom4([4]byte(packet[16:20])).String()
	c, e := b.dial(3, target, 0)
	if e != nil {
		log.Printf("内网真实回显通道失败：%v", e)
		return
	}
	defer b.release(c)
	c.SetDeadline(time.Now().Add(4 * time.Second))
	data := packet[h+8:]
	if len(data) > 65000 {
		return
	}
	request := []byte{packet[8], 0}
	if packet[6]&0x40 != 0 {
		request[1] = 2
	}
	request = binary.LittleEndian.AppendUint16(request, uint16(len(data)))
	request = append(request, data...)
	if send(c, request) != nil {
		return
	}
	header := make([]byte, 15)
	if _, e = io.ReadFull(c, header); e != nil {
		return
	}
	status := binary.LittleEndian.Uint32(header)
	if status != 0 {
		log.Printf("内网真实回显未收到成功响应：%s，状态 %d", target, status)
		return
	}
	size := int(binary.LittleEndian.Uint16(header[13:15]))
	if size > 65000 {
		return
	}
	actual := make([]byte, size)
	if _, e = io.ReadFull(c, actual); e != nil {
		return
	}
	// 仅依据 B 的真实 ICMP 回包生成应答；超时和失败绝不产生本地成功回包。
	reply := make([]byte, 28+len(actual))
	reply[0] = 0x45
	binary.BigEndian.PutUint16(reply[2:4], uint16(len(reply)))
	copy(reply[4:6], packet[4:6])
	reply[8] = header[12]
	reply[9] = 1
	copy(reply[12:16], header[4:8])
	copy(reply[16:20], packet[12:16])
	copy(reply[24:28], packet[h+4:h+8])
	copy(reply[28:], actual)
	binary.BigEndian.PutUint16(reply[22:24], checksum(reply[20:]))
	binary.BigEndian.PutUint16(reply[10:12], checksum(reply[:20]))
	if e = write(reply); e != nil {
		log.Printf("写入真实回显应答失败：%v", e)
	}
}
