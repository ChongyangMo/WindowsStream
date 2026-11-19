package main

import (
	"net"
	"time"
)

func watchConsent(device string, initial bool, local net.Conn) func() {
	stop := make(chan struct{})
	done := make(chan struct{})
	go func() {
		defer close(done)
		ticker := time.NewTicker(200 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-stop:
				return
			case <-ticker.C:
				if consentReady(device) != initial {
					_ = local.Close()
					return
				}
			}
		}
	}()
	return func() {
		close(stop)
		<-done
	}
}
