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
- 管理员被控端 `windows/build/2release/host/WindowsStream-Host-Admin.exe`

每次构建按 UTC 自动生成被控端版号。EXE 的「属性 → 详细信息」、被控端主窗口和托盘提示、控制端连接日志均显示该版号。

被控端启动时会为当前 Windows 用户注册保活计划任务。守护进程每 10 秒检查一次被控端，计划任务在登录时和每分钟补启动守护。通过主窗口或托盘主动退出时需要输入退出密码。验证通过后暂停本次登录会话的保活，手动打开被控端后恢复。是否在下次登录时启动仍由「开机自启动」决定。

控制端右键托盘图标，打开「网络配置」，可以配置网线直连、放行或撤销 VMware 虚拟机代理的防火墙规则。选择操作后确认 Windows 管理员权限提示。配置逻辑内置于控制端 EXE。

在 Visual Studio 打开 `WindowsStream.slnx`，选择 `Release | x64` 并生成解决方案。也可以在 Visual Studio 开发人员终端运行以下命令。

```powershell
msbuild WindowsStream.slnx /m /p:Configuration=Release /p:Platform=x64
```

## 后台读取日志

更新控制端和被控端。在被控端配置允许读取的目录，然后重新建立 WindowsStream 会话。统一版在用户的 `AppData\Local\WindowsStream\RelayHost` 下读取 `remote-files.ini`，配置跟随同一用户保留。便携版默认读取程序旁的 `host.ini`，也可在 `[permissions]` 中用 `remote_files_ini` 指定另一份配置。

```ini
[remote_files]
vito=C:\lvi\log
```

使用已连接会话的 `client.ini`。主连接使用控制端目录下的配置，其他会话使用各自的配置。

```cmd
start "" /wait WindowsStream.exe --remote-files "C:\path\to\client.ini" list - "C:\Logs\remote-roots.txt"
start "" /wait WindowsStream.exe --remote-files "C:\path\to\client.ini" list vito "C:\Logs\vito-files.tsv"
start "" /wait WindowsStream.exe --remote-files "C:\path\to\client.ini" tail "vito/Vito_20261119.lvilog" "C:\Logs\Vito_20261119.lvilog"
```

`list` 列出目录别名或按修改时间排列的最新 200 个日志。列表包含文件名、字节数和 UTC 修改时间。`read` 读取完整文件，`tail` 最多读取文件末尾 4 MiB，保留文件字节。支持目录内的 `.log`、`.lvilog` 和 `.txt`。输出路径需要提前创建父目录，命令在输出已存在时返回错误。

管理员权限运行的被控端默认开启文字剪贴板和双向文件拖拽。需要调整时，在被控端 `host.ini` 的 `[administrator_permissions]` 设置 `clipboard` 或 `file_drag`。

中转机使用原来的 Windows 管理员账号运行最新版 `WindowsStream-Host-Admin.exe`，确认现场首次安装。控制端更新后继续用原配对连接。出现 UAC 时，等画面重连并显示「UAC 加密输入」，再手动点击确认或输入 Windows 要求的管理员凭据。
