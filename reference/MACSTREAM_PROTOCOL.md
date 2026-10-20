# MacStream 协议 v1

TCP，默认端口 7300，两端都关闭 Nagle。所有整数为小端序。

## 消息帧

```
u32 payloadLength
u8  type
u8[payloadLength] payload
```

## 握手

1. 服务端 → `1 ServerHello`：`u32 version, u8[32] nonce`
2. 客户端 → `2 ClientAuth`：`u8[32] HMAC-SHA256(key = token 的 UTF-8 字节, data = nonce), u32 version, u16 viewWidth, u16 viewHeight, u16 monitorWidth, u16 monitorHeight, u16 refreshHz, u8 wireless`
   - wireless：客户端到 Mac 的路由走无线网卡，Mac 端自适应码率起步上限 100 Mbps（有线 250 Mbps）
   - view：客户区物理像素，Mac 端输出画面不会超过它
   - monitor + refresh：客户端显示器，Mac 端据此创建“看起来像 monitor 尺寸”的 HiDPI 虚拟屏并以 refresh Hz 刷新，其他显示器镜像它（可在配置中关闭）
3. 服务端 → `3 AuthResult`：`u8 ok, u16 width, u16 height, u16 fps`（失败时 ok=0 并断开）

token 保存在 Mac 的 `~/Library/Application Support/MacStream/config.json`，
Windows 端保存在 `%APPDATA%\MacStream\client.ini`。

## 服务端 → 客户端

| type | 名称 | payload |
|---|---|---|
| 10 | Video | `u64 captureMicros, u8 flags(bit0=关键帧, bit1=静止补清晰帧, bit2=带变化图块), [变化图块], H.264 Annex-B` |
| 12 | Lossless | `u32 serial, u16 frameW, u16 frameH, u16 tileSize, u16 count, u8 flags(bit0=本轮最后一批)`，每块 `u16 tx, u16 ty, u32 len(bit31=未压缩), LZ4 块数据（BGRA）` |
| 11 | Cursor | `u16 w, u16 h, u16 hotX, u16 hotY, BGRA 预乘 w*h*4` |
| 21 | Pong | `u64 clientMicros, u64 serverMicros` |
| 22 | Stats | `u32 码率上限 kbps, u16 帧率上限（1000 = 不限）, u16 网络排队 ms, u32 实测链路 kbps`，每秒一次 |
| 30 | Clipboard | UTF-8 文本 |

## 客户端 → 服务端

| type | 名称 | payload |
|---|---|---|
| 20 | Ping | `u64 clientMicros` |
| 30 | Clipboard | UTF-8 文本 |
| 40 | MouseMove | `u16 x, u16 y`（0–65535 归一化到 Mac 屏幕） |
| 41 | MouseButton | `u8 button(0左 1右 2中 3后退 4前进), u8 down, u16 x, u16 y` |
| 42 | Scroll | `i16 dx, i16 dy`（Windows 滚轮单位，120 = 一格） |
| 43 | Key | `u16 macVirtualKeyCode, u8 down` |
| 44 | ReleaseAll | 空 |
| 45 | ViewSize | `u16 width, u16 height`（客户区物理像素，Mac 按它重新输出） |
| 46 | MonitorMode | `u16 width, u16 height, u16 refreshHz`（窗口所在显示器变了：换屏、改分辨率或拖到另一块屏，Mac 按它重建虚拟屏） |
| 50 | RequestKeyframe | 空 |

视频只在画面变化时发送；发送队列积压时服务端丢弃非关键帧，并让下一帧强制编码为关键帧。

## 静止无损

画面按 64×64 图块划分。每个视频帧附带本帧变化的图块：
`u32 serial, u16 tileSize, u16 tilesX, u16 tilesY, 位图（行优先，bit i = 图块 i）`。
客户端收到后让这些图块退回视频显示，并记录该图块最近一次变化的 serial。

画面静止 120 ms 后，服务端把自上次无损以来变化过的图块以 Lossless 消息分批发送（每批约 512 KB，写完一批再发下一批；画面一变就中止）。
客户端先暂存一轮里的各批图块，收到 `flags.bit0` 的最后一批才让整轮一起生效；中途被中止的一轮不会生效，服务端会整轮重发。
生效时只接受 `serial ≥ 该图块最近变化 serial` 的图块，过期数据直接丢弃，保证不会把旧像素盖在新内容上。

视频使用 H.264 全范围 YUV（BT.709 矩阵、sRGB 传输特性），由 Mac 端显式从 BGRA 转换，与无损层的 RGB 只差压缩误差。
被丢弃视频帧的变化图块会并入下一帧发送。
