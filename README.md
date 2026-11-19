# WindowsStream

## 构建目录

| 目录 | 用途 |
| --- | --- |
| `windows/build/1debug` | Debug 调试产物 |
| `windows/build/2release` | Release 正式产物和打包组件 |
| `windows/build/obj` | 编译中间文件、配置和打包数据 |

项目的 EXE 只放在 `1debug` 和 `2release` 下。调试和验证使用现有目录，构建时沿用项目默认的 `OutDir`。`obj` 不放独立的 EXE。

发现额外的 EXE 目录时，先核对实际用于编译的源码和当前项目。如果旧目录的改动仍缺失，合入当前源码后重新构建 Release。确认产物已更新后，将旧目录移到回收站。

## 正式版

- 控制端 `windows/build/2release/controller/WindowsStream.exe`
- 被控端 `windows/build/2release/host/WindowsStream-Host.exe`

控制端右键托盘图标，打开「网络配置」，可以配置网线直连、放行或撤销 VMware 虚拟机代理的防火墙规则。选择操作后确认 Windows 管理员权限提示。配置逻辑内置于控制端 EXE。

在 Visual Studio 打开 `WindowsStream.slnx`，选择 `Release | x64` 并生成解决方案。也可以在 Visual Studio 开发人员终端运行以下命令。

```powershell
msbuild WindowsStream.slnx /m /p:Configuration=Release /p:Platform=x64
```
