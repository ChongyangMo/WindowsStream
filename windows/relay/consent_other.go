//go:build !windows

package main

func consentReady(string) bool { return false }
