package main

import (
	"crypto/sha256"
	"fmt"

	"golang.org/x/sys/windows"
)

func consentReady(device string) bool {
	identity := sha256.Sum256([]byte(device))
	name := windows.StringToUTF16Ptr(fmt.Sprintf("Global\\WindowsStreamConsent-%x", identity))
	event, err := windows.OpenEvent(windows.SYNCHRONIZE|windows.READ_CONTROL, false, name)
	if err != nil {
		return false
	}
	defer windows.CloseHandle(event)
	result, err := windows.WaitForSingleObject(event, 0)
	if err != nil || result != windows.WAIT_OBJECT_0 {
		return false
	}
	descriptor, err := windows.GetSecurityInfo(event, windows.SE_KERNEL_OBJECT, windows.OWNER_SECURITY_INFORMATION)
	if err != nil {
		return false
	}
	owner, _, err := descriptor.Owner()
	return err == nil && owner != nil && owner.IsWellKnown(windows.WinLocalSystemSid)
}
