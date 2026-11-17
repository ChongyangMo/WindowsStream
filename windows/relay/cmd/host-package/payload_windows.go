//go:build windows && hostpackage

package main

import (
	"archive/zip"
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

//go:embed payload.zip
var payload []byte

func installPayload(root string) (string, error) {
	digest := sha256.Sum256(payload)
	app := filepath.Join(root, "runtime-"+hex.EncodeToString(digest[:12]))
	if entry, err := os.Stat(app); err == nil && entry.IsDir() {
		return app, nil
	}
	archive, err := zip.NewReader(bytes.NewReader(payload), int64(len(payload)))
	if err != nil {
		return "", err
	}
	stage, err := os.MkdirTemp(root, "runtime-stage-")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(stage)
	for _, entry := range archive.File {
		if entry.Name != filepath.Base(entry.Name) || !entry.Mode().IsRegular() {
			return "", fmt.Errorf("被控端包内文件无效")
		}
		if err := unpackFile(entry, stage); err != nil {
			return "", err
		}
	}
	if err := os.Rename(stage, app); err != nil {
		return "", err
	}
	return app, nil
}

func unpackFile(entry *zip.File, folder string) error {
	reader, err := entry.Open()
	if err != nil {
		return err
	}
	defer reader.Close()
	file, err := os.OpenFile(filepath.Join(folder, entry.Name), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	_, err = io.Copy(file, reader)
	return errors.Join(err, file.Close())
}

func configureIdentity(root, source, app string) error {
	const name = "identity.ini"
	stable := filepath.Join(root, name)
	identity, err := os.ReadFile(stable)
	if errors.Is(err, os.ErrNotExist) {
		identity, err = os.ReadFile(filepath.Join(source, name))
		if errors.Is(err, os.ErrNotExist) {
			identity, err = newIdentity(), nil
		}
		if err != nil {
			return err
		}
		if err := writeAtomic(stable, identity); err != nil {
			return err
		}
	} else if err != nil {
		return err
	}
	path := filepath.Join(app, name)
	if current, err := os.ReadFile(path); err == nil && bytes.Equal(current, identity) {
		return nil
	}
	return writeAtomic(path, identity)
}

func newIdentity() []byte {
	var data bytes.Buffer
	data.WriteString("[identity]\r\n")
	for _, key := range []string{"host_key", "viewer_key", "stream_key"} {
		var secret [32]byte
		_, _ = rand.Read(secret[:])
		fmt.Fprintf(&data, "%s=%x\r\n", key, secret)
	}
	return data.Bytes()
}

func writeNew(path string, data []byte) error {
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	_, err = file.Write(data)
	if err := errors.Join(err, file.Close()); err != nil {
		_ = os.Remove(path)
		return err
	}
	return nil
}

func writeAtomic(path string, data []byte) error {
	file, err := os.CreateTemp(filepath.Dir(path), "windowsstream-config-")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	_, err = file.Write(data)
	if err := errors.Join(err, file.Close()); err != nil {
		return err
	}
	return os.Rename(file.Name(), path)
}
