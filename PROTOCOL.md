# WindowsStream 协议 v2

独立于 MacStream v1。默认 TCP 7400、UDP 7401；整数小端序，启用 TCP_NODELAY。

## 连接与认证

控制角色与 TCP 建连方向独立：

- 控制端 `host=listen`：等待 `mode=reverse` 的被控端主动连接。
- 控制端 `host=固定IPv4地址` 或 `host=auto`：主动连接 `mode=listen` 的被控端。
- 反向模式由 `host.ini` 的 `controller_host` 指定初始控制端地址，连不上时按下文“反向模式寻找控制端”自动发现。

消息帧：`u32 payloadLength, u8 type, u8[payloadLength] payload`，长度不含类型。

1. 被控端发送 `1 ServerHello`：`u32 version, u8[32] nonce, u8[32] proof`，共 68 字节。proof 为 `HMAC-SHA256(token, UTF8("windowsstream-host-v2") + nonce)`。
2. 控制端校验版本和 proof 后发送 `2 ClientAuth`：`u8[32] HMAC-SHA256(token, nonce), u32 version, u16 viewWidth, u16 viewHeight, u16 monitorWidth, u16 monitorHeight, u16 refreshHz, u8 wireless`，共 47 字节。
3. 被控端发送 `3 AuthResult`：`u8 ok, u16 width, u16 height, u16 fps`。认证失败则断开。

token 是两端 INI 中相同的 UTF-8 字符串。本版只实现认证，消息体未加密。

## 被控端发送

| type | 内容 |
|---|---|
| 10 Video | `u64 captureMicros, u8 flags(bit0=关键帧), H.264 Annex-B` |
| 11 Cursor | `u16 width, height, hotX, hotY, BGRA 预乘数据`；系统指针独立于桌面视频发送 |
| 21 Pong | `u64 clientMicros, u64 hostMicros` |
| 23 HostInfo | UTF-8 诊断文本，控制端最大接受 4096 字节：真实设备信息及阶段性能统计 |
| 30 Clipboard | UTF-8 文本，最大 4 MiB |

视频为 BT.709 有限范围 NV12，优先通过 GPU 转换和硬件 H.264 编码器输出；失败时可回退到系统软件编码器。默认请求 100 fps，协商上限受源屏幕刷新率和编码器能力限制。时间戳由 QueryPerformanceCounter 换算微秒。

## 控制端发送

| type | 内容 |
|---|---|
| 20 Ping | `u64 clientMicros` |
| 30 Clipboard | UTF-8 文本 |
| 40 MouseMove | `u16 x, u16 y`，归一化到被控端主屏 |
| 41 MouseButton | `u8 button(0左/1右/2中/3后退/4前进), u8 down, u16 x, u16 y` |
| 42 Scroll | `i16 dx, i16 dy`，120 为一格 |
| 43 Key | `u16 scanCode, u8 down`；bit8 为 E0，Pause 用 bit9 加虚拟键码 |
| 44 ReleaseAll | 空，释放当前会话按下的键鼠 |
| 45 ViewSize | `u16 width, u16 height`，首版不调整被控端屏幕 |
| 46 MonitorMode | `u16 width, u16 height, u16 refreshHz`，首版不创建虚拟屏 |
| 50 RequestKeyframe | 空 |

控制消息限制 4 MiB，画面消息限制 32 MiB。认证有超时，断线释放本次会话按下的键鼠。

## 发现

请求 `WSDISC2 + 16字节随机数`。

应答 `WSHERE2 + u16 TCP端口 + HMAC-SHA256(token, UTF8("windowsstream-discover") + 随机数) + UTF8电脑名`。

组播 `239.255.77.87`，被控端仅在 `mode=listen` 时应答。

### 反向模式寻找控制端

控制端 `host=listen` 时在 UDP 7401 应答。被控端只从本机 169.254 地址向 `169.254.255.255` 与 `255.255.255.255` 广播，并单播询问已知地址：

- 请求 55 字节：`WSFIND2 + u8[16] nonce + HMAC-SHA256(token, UTF8("windowsstream-find-controller") + nonce)`。
- 应答 41 字节：`WSCTRL2 + u16 tcpPort + HMAC-SHA256(token, UTF8("windowsstream-controller-here") + nonce + u16 tcpPort)`。

签名不对的请求不应答。`bind_host=auto` 时控制端只应答、只接受 169.254.0.0/16 来源。被控端以应答的来源地址作为控制端地址，TCP 端口仍用配置值。

## 独立升级通道

控制端默认在绑定地址的 TCP 7402 提供 HTTP，升级服务独立于桌面会话。每次请求携带随机 16 字节 nonce 的小写十六进制 `X-WS-Nonce`，及 `X-WS-Proof`。后者为 HMAC-SHA256(token, UTF8("windowsstream-update-http-v1\n" + method + "\n" + path + "\n" + nonce + "\n" + hex(SHA256(body)))) 的小写十六进制。

- GET `/manifest`：小端 `u32 schema=1, u64 exeBytes, u32 PE TimeDateStamp, u8[32] SHA256, u8[32] signature`。signature=HMAC-SHA256(token, UTF8("windowsstream-package-v1") + 清单前48字节)。
- GET `/package/<sha256>`：返回上一次有效清单对应的完整 x64 PE 程序，最大 64 MiB。
- POST `/diagnostics`：不超过 64 KiB 的真实 host.log 增量，认证后写入 A 当前日志。

仅安装构建时间更晚且哈希不同的程序。替换助手验证清单、实际程序以及原进程路径和创建时间，原位原子替换并保留旧文件。INI 不在远程更新载荷内。更新后进程完成升级服务身份校验后写健康回执；30秒无回执则回退并记录该包为失败包。

## 独立内网代理通道

默认 A 监听 TCP 7403，B 主动连接；与视频消息帧完全独立。A 的本机 `127.0.0.1:1080` 接受 HTTP 绝对地址请求、HTTPS CONNECT、SOCKS5 无认证 CONNECT，入口不向局域网开放。SOCKS5 不支持 BIND 和 UDP ASSOCIATE。

双向认证：A 发送8字节 `WSPXY1\r\n` 和32字节随机数；B 回复32字节随机数和32字节 `HMAC-SHA256(token, UTF8("windowsstream-proxy-host-v1") + A完整40字节hello + B随机数)`。A 校验后发送 `HMAC-SHA256(token, UTF8("windowsstream-proxy-controller-v1") + A完整40字节hello + B随机数)`，B 校验后回复单字节1。失败关闭，认证成功的空闲连接才可分配请求。

请求：A 发送 `u8 command=1, u16 targetPort, u16 hostBytes, ASCII[hostBytes] host`，整数为小端序。host 为 IPv4、IPv6 或域名，长度1到253；域名由 B 通过系统解析器解析。B 连接后回复单字节 SOCKS5 状态码（0成功、3网络不可达、4地址不可达或解析失败、5拒绝连接），失败关闭。成功后双方原样双向传输 TCP 字节直到关闭，支持 TCP 半关闭。A 不在收到成功状态前向本机客户端报告成功。

B 默认维护16条独立连接，空闲等待目标，转发结束或断线后重新连接；连接和认证失败时约5秒后重试。数据转发采用每方向64KiB缓冲区，慢接收方背压，退出时统一 shutdown。通道只认证、不额外加密，适用于当前受信任网线网络。

### 真实 ICMP 回显

本机回环代理额外接受内部请求 `u8 127, u8 command`。`command=3` 随后为小端 `u16 port=0, u16 hostBytes, ASCII IPv4`，目标仅允许 `10.18.0.0/16`。A 使用已认证的 B 通道转发 `u8 3` 及相同的目标字段。B 创建真实 ICMP 句柄后回复单字节0，无法创建则回复非0并关闭；此字节只表示接口已打开，不代表目标可达。

请求正文为 `u8 TTL, u8 flags, u16 payloadBytes, payload`，最多65000字节。B 使用 IcmpSendEcho 发送真实 ICMP，等待最多2000ms。返回小端 `u32 WindowsStatus, IPv4[4] source, u32 roundTripMs, u8 replyTTL, u16 payloadBytes, payload`；IPv4地址按网络字节顺序存储。成功状态为0；失败保留真实 Windows 状态，不产生本地成功回包。耗时为 B 到目标的往返。

A 的 Ping 客户端每次发送32字节随机数据，成功时检查来源、长度和数据完全一致；未知 TTL 显示未知。`command=4` 为能力查询，无目标字段；B 回复 `u8 0, u32 flags`，当前版本高16位为1，低位1/2/4分别表示 TCP、UDP 套接字和 ICMP 接口可创建，不代表任何目标连通。标准 HTTP/SOCKS5 请求格式不变。

## 结构化诊断 type 24

被控端在认证成功后以及每两秒发送64字节，均为小端：`u32 schema=1`，随后六个 `u16`（源宽、源高、源Hz、输出宽、输出高、协商fps），`u8 pipeline`（1 GPU硬编，2 GPU软编，3 GDI软编），三个保留零字节；再跟十一个 `u32`（采样毫秒、采集毫帧/秒、提交毫帧/秒、输出毫帧/秒、视频kbps、平均采集微秒、平均提交微秒、平均采集至编码输出微秒、平均发送微秒、背压跳过次数、待编码数）。无耗时样本使用UINT32_MAX；初始消息采样毫秒为0。旧控制端可忽略该类型。
