//go:build windows && hostpackage && !hostadmin

package main

import "fmt"

func launchVariant(root, source string, configured bool) (string, error) {
	app, err := installPayload(root)
	if err != nil {
		return "", fmt.Errorf("无法准备被控端程序：%w", err)
	}
	if configured {
		return "", startHost(app, source, false)
	}
	if err := configureIdentity(root, source, app); err != nil {
		return "", fmt.Errorf("无法准备本机配对：%w", err)
	}
	if err := startHost(app, "", false); err != nil {
		return "", err
	}
	return exportPair(app, source, root)
}
