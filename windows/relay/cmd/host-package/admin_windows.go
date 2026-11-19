//go:build windows && hostpackage && hostadmin

package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"unsafe"

	"golang.org/x/sys/windows"
)

func launchVariant(userRoot, source string, configured bool) (string, error) {
	root, err := adminDirectory()
	if err != nil {
		return "", fmt.Errorf("无法准备管理员版安装目录：%w", err)
	}
	mode := "universal"
	if configured {
		mode = "configured"
	}
	root = filepath.Join(root, mode)
	if err := os.MkdirAll(root, 0700); err != nil {
		return "", err
	}
	app, err := installPayload(root)
	if err != nil {
		return "", fmt.Errorf("无法准备管理员版被控端：%w", err)
	}
	if configured {
		for _, name := range []string{"host.ini", "relay.ini"} {
			data, err := os.ReadFile(filepath.Join(source, name))
			if name == "relay.ini" && errors.Is(err, os.ErrNotExist) {
				if err := os.Remove(filepath.Join(app, name)); err != nil && !errors.Is(err, os.ErrNotExist) {
					return "", err
				}
				continue
			}
			if err != nil {
				return "", err
			}
			if err := writeAtomic(filepath.Join(app, name), data); err != nil {
				return "", err
			}
		}
		return "", startHost(app, "", true)
	}
	if err := configureIdentity(userRoot, source, app); err != nil {
		return "", fmt.Errorf("无法沿用本机配对：%w", err)
	}
	if data, err := os.ReadFile(filepath.Join(userRoot, "remote-files.ini")); err == nil {
		if err := writeAtomic(filepath.Join(root, "remote-files.ini"), data); err != nil {
			return "", err
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return "", err
	}
	if err := startHost(app, "", true); err != nil {
		return "", err
	}
	return exportPair(app, source, userRoot)
}

func adminDirectory() (string, error) {
	programFiles, err := windows.KnownFolderPath(windows.FOLDERID_ProgramFiles, 0)
	if err != nil {
		return "", err
	}
	root := filepath.Join(programFiles, "WindowsStreamRelayAdmin")
	descriptor, err := windows.SecurityDescriptorFromString("O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)")
	if err != nil {
		return "", err
	}
	attributes := windows.SecurityAttributes{
		Length:             uint32(unsafe.Sizeof(windows.SecurityAttributes{})),
		SecurityDescriptor: descriptor,
	}
	path := windows.StringToUTF16Ptr(root)
	err = windows.CreateDirectory(path, &attributes)
	if err != nil && !errors.Is(err, windows.ERROR_ALREADY_EXISTS) {
		return "", err
	}
	flags, err := windows.GetFileAttributes(path)
	if err != nil {
		return "", err
	}
	if flags&windows.FILE_ATTRIBUTE_REPARSE_POINT != 0 || flags&windows.FILE_ATTRIBUTE_DIRECTORY == 0 {
		return "", fmt.Errorf("管理员版安装目录无效")
	}
	acl, _, err := descriptor.DACL()
	if err != nil {
		return "", err
	}
	owner, _, err := descriptor.Owner()
	if err != nil {
		return "", err
	}
	err = windows.SetNamedSecurityInfo(root, windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION|windows.PROTECTED_DACL_SECURITY_INFORMATION,
		owner, nil, acl, nil)
	return root, err
}
