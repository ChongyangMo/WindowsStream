//go:build windows && hostpackage

package main

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"

	"golang.org/x/sys/windows"
)

const pairName = "WindowsStream-Pair.ini"

func main() {
	fallback, err := launch()
	if err != nil {
		messageBox(err.Error(), windows.MB_ICONERROR)
		os.Exit(1)
	}
	if fallback != "" {
		messageBox("被控端已启动。可在此处取得配对文件：\r\n"+fallback, windows.MB_ICONINFORMATION)
	}
}

func launch() (string, error) {
	executable, err := os.Executable()
	if err != nil {
		return "", fmt.Errorf("无法读取被控端位置：%w", err)
	}
	source := filepath.Dir(executable)
	configured, err := hasHostConfig(source)
	if err != nil {
		return "", err
	}
	appData, err := windows.KnownFolderPath(windows.FOLDERID_LocalAppData, windows.KF_FLAG_CREATE)
	if err != nil {
		return "", fmt.Errorf("无法读取本机应用数据目录：%w", err)
	}
	root := filepath.Join(appData, "WindowsStream", "RelayHost")

	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	mutex, err := acquireLaunchMutex()
	if err != nil {
		return "", err
	}
	defer windows.CloseHandle(mutex)
	defer windows.ReleaseMutex(mutex)

	if err := os.MkdirAll(root, 0700); err != nil {
		return "", err
	}
	app, err := installPayload(root)
	if err != nil {
		return "", fmt.Errorf("无法准备被控端程序：%w", err)
	}
	if configured {
		return "", startHost(app, source)
	}
	if err := configureIdentity(root, source, app); err != nil {
		return "", fmt.Errorf("无法准备本机配对：%w", err)
	}
	if err := startHost(app, ""); err != nil {
		return "", err
	}
	return exportPair(app, source, root)
}

func hasHostConfig(source string) (bool, error) {
	for _, name := range []string{"host.ini", "relay.ini"} {
		info, err := os.Stat(filepath.Join(source, name))
		if err == nil {
			if !info.Mode().IsRegular() {
				return false, fmt.Errorf("%s 必须是配置文件", name)
			}
			return true, nil
		}
		if !errors.Is(err, os.ErrNotExist) {
			return false, fmt.Errorf("无法读取 %s：%w", name, err)
		}
	}
	return false, nil
}

func acquireLaunchMutex() (windows.Handle, error) {
	name := windows.StringToUTF16Ptr(`Local\WindowsStreamHostPackage`)
	handle, err := windows.CreateMutex(nil, false, name)
	if err != nil && !errors.Is(err, windows.ERROR_ALREADY_EXISTS) {
		return 0, fmt.Errorf("无法启动被控端：%w", err)
	}
	status, err := windows.WaitForSingleObject(handle, 30000)
	if err != nil || (status != windows.WAIT_OBJECT_0 && status != windows.WAIT_ABANDONED) {
		_ = windows.CloseHandle(handle)
		return 0, fmt.Errorf("另一个 WindowsStream 被控端正在启动，请稍后重试")
	}
	return handle, nil
}

func startHost(app, installTo string) error {
	system, err := windows.GetSystemDirectory()
	if err != nil {
		return err
	}
	powershell := filepath.Join(system, "WindowsPowerShell", "v1.0", "powershell.exe")
	args := []string{"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", filepath.Join(app, "start-host.ps1"), "-Package"}
	if installTo != "" {
		args = append(args, "-InstallTo", installTo)
	}
	command := exec.Command(powershell, args...)
	command.Dir = app
	command.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: windows.CREATE_NO_WINDOW}
	output, err := command.CombinedOutput()
	if err != nil {
		detail := strings.TrimSpace(strings.ToValidUTF8(string(output), ""))
		if detail != "" {
			return fmt.Errorf("被控端启动失败：\r\n%s", detail)
		}
		return fmt.Errorf("被控端启动失败：%w", err)
	}
	return nil
}

func exportPair(app, source, root string) (string, error) {
	pair, err := os.ReadFile(filepath.Join(app, pairName))
	if err != nil {
		return "", fmt.Errorf("无法读取本机配对文件：%w", err)
	}
	out := filepath.Join(source, pairName)
	existing, err := os.ReadFile(out)
	if err == nil && bytes.Equal(existing, pair) {
		return "", nil
	}
	if errors.Is(err, os.ErrNotExist) && writeNew(out, pair) == nil {
		return "", nil
	}
	fallback := filepath.Join(root, pairName)
	return fallback, writeAtomic(fallback, pair)
}

func messageBox(message string, icon uint32) {
	text, err := windows.UTF16PtrFromString(message)
	if err == nil {
		_, _ = windows.MessageBox(0, text, windows.StringToUTF16Ptr("WindowsStream"), icon|windows.MB_OK)
	}
}
