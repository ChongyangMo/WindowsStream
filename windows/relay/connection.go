package main

import (
	"bufio"
	"crypto/tls"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"time"

	"golang.org/x/net/websocket"
)

type bufferedConnection struct {
	net.Conn
	reader *bufio.Reader
}

func (c bufferedConnection) Read(p []byte) (int, error) { return c.reader.Read(p) }

func openWebSocket(c config) (*websocket.Conn, error) {
	e, _ := url.Parse(c.endpoint)
	config, err := websocket.NewConfig(c.endpoint, "https://"+e.Host)
	if err != nil {
		return nil, err
	}
	config.Header.Set("Authorization", "Bearer "+c.token)
	config.Header.Set("X-WindowsStream-Role", c.role)
	config.Header.Set("X-WindowsStream-Device", c.device)

	address := c.server
	if c.httpProxy != "" {
		address = c.httpProxy
	}
	conn, err := net.DialTimeout("tcp", address, 4*time.Second)
	if err != nil {
		return nil, err
	}
	connected := false
	defer func() {
		if !connected {
			_ = conn.Close()
		}
	}()
	_ = conn.SetDeadline(time.Now().Add(5 * time.Second))
	if c.httpProxy != "" {
		if _, err := fmt.Fprintf(conn, "CONNECT %s HTTP/1.1\r\nHost: %s\r\n\r\n", c.server, c.server); err != nil {
			return nil, err
		}
		reader := bufio.NewReader(conn)
		response, err := http.ReadResponse(reader, &http.Request{Method: http.MethodConnect})
		if err != nil {
			return nil, err
		}
		if response.StatusCode != http.StatusOK {
			return nil, fmt.Errorf("内网代理返回 HTTP %d", response.StatusCode)
		}
		conn = bufferedConnection{Conn: conn, reader: reader}
	}
	secure := tls.Client(conn, &tls.Config{
		ServerName: e.Hostname(), MinVersion: tls.VersionTLS12, NextProtos: []string{"http/1.1"},
	})
	if err := secure.Handshake(); err != nil {
		return nil, err
	}
	socket, err := websocket.NewClient(config, secure)
	if err != nil {
		return nil, err
	}
	socket.PayloadType = websocket.BinaryFrame
	_ = socket.SetDeadline(time.Time{})
	connected = true
	return socket, nil
}

const (
	paired byte = iota + 1
	offline
	busy
	waitExpired
)

func waitStatus(socket *websocket.Conn, timeout time.Duration) (byte, error) {
	_ = socket.SetReadDeadline(time.Now().Add(timeout))
	var data [1]byte
	_, err := io.ReadFull(socket, data[:])
	_ = socket.SetReadDeadline(time.Time{})
	return data[0], err
}

func bridge(local net.Conn, remote *websocket.Conn) {
	stop := func() {
		_ = local.Close()
		_ = remote.SetDeadline(time.Now())
	}
	done := make(chan struct{})
	go func() {
		_, _ = io.Copy(local, remote)
		stop()
		close(done)
	}()
	_, _ = io.Copy(remote, local)
	stop()
	<-done
}
