import Compression
import CoreMedia
import Foundation
import ScreenCaptureKit

/// 本帧相对上一帧变化的 64×64 图块（位图，行优先）。serial 是内容序号，客户端据此丢弃过期的无损数据。
struct TileMask {
    let serial: UInt32
    let tilesX: Int
    let tilesY: Int
    let bits: [UInt8]
}

struct EncodedVideo {
    let data: [UInt8]
    let keyframe: Bool
    let refine: Bool
    let captureMicros: UInt64
    let dirty: TileMask?
}

/// ScreenCaptureKit 采集主屏（BGRA、sRGB、不含鼠标指针）：
/// - 视频：交给 VideoEncoder 低延迟编码；每帧附带变化图块，客户端据此让这些块退回视频显示。
/// - 无损：画面静止后，把变化过的图块用 LZ4 无损发送，客户端覆盖显示，达到像素级清晰。
/// - 输出尺寸按客户端窗口（物理像素）适配，保证客户端 1:1 显示；显示器变化时自动重新采集。
final class ScreenCapture: NSObject, SCStreamOutput, SCStreamDelegate {
    typealias VideoSink = (EncodedVideo) -> Bool
    /// 发送一批无损图块；done 在写入套接字后调用。返回 false 表示连接已断开。
    typealias LosslessSink = (_ payload: [UInt8], _ done: @escaping () -> Void) -> Bool

    static let tileSize = 64

    private let refineDelay = 0.05
    private let refineFrames = 6
    private let losslessDelay = 0.12
    /// 无损数据分小批发送：每批发之前要求网络积压已清空，画面一动立刻停下，绝不耽误视频（Wi-Fi 下尤其重要）。
    private let losslessBatchBytes = 128 * 1024
    private let losslessDrainedBytes = 96 * 1024
    private var losslessReady: () -> Bool = { true }

    private let queue = DispatchQueue(label: "macstream.capture", qos: .userInteractive)
    private let losslessQueue = DispatchQueue(label: "macstream.lossless", qos: .userInitiated)
    private let maxFps: Int
    private let bitrateMbps: Int
    private let maxQP: Int
    private let losslessEnabled: Bool

    private var stream: SCStream?
    private var encoder: VideoEncoder?
    private var lastFrame: CVPixelBuffer?
    private var frameSerial: UInt32 = 0
    private var videoSink: VideoSink?
    private var losslessSink: LosslessSink?
    private var generation = 0
    private var currentFps = 60
    private var viewWidth = 0
    private var viewHeight = 0
    private var reconfigurePending = false

    private var tilesX = 0
    private var tilesY = 0
    private var dirtySinceLossless: [Bool] = []
    /// 按提交编码的顺序排队的待发视频帧：编码完成且（需要时）图块比对完成后，从队头依次发出。
    private struct Outgoing {
        let micros: UInt64
        let created: UInt64
        let needsMask: Bool
        var video: EncodedVideo?
        var mask: TileMask?
    }
    private var outbox: [Outgoing] = []
    private var losslessInFlight = false

    private let serialLock = NSLock()
    private var latestSerial: UInt32 = 0
    private let gpuDiffer = TileDiffer()

    // 性能统计（采集队列上访问），有画面变化时每 2 秒写一次日志。
    private var statCaptured = 0
    private var statEncoded = 0
    private var statDropped = 0
    private var statHandlerMicros: UInt64 = 0
    private var statPipelineMicros: UInt64 = 0
    private var statBytes = 0
    private var statSince = Clock.nowMicros()

    private func reportStats() {
        let now = Clock.nowMicros()
        guard now - statSince >= 2_000_000 else { return }
        let seconds = Double(now - statSince) / 1e6
        if statCaptured > 0 {
            let enc = encoder?.takeStats() ?? (frames: 0, wait: 0, convert: 0, encode: 0)
            Log.info(String(format: "性能 采集 %.1f fps，送出 %.1f fps，丢弃 %d，采集回调 %.2f ms/帧，显示→编码完成 %.2f ms" +
                            "（排队 %.2f，转换 %.2f，编码 %.2f），%.1f Mbps",
                            Double(statCaptured) / seconds, Double(statEncoded) / seconds, statDropped,
                            Double(statHandlerMicros) / 1000 / Double(statCaptured),
                            statEncoded > 0 ? Double(statPipelineMicros) / 1000 / Double(statEncoded) : 0,
                            enc.wait, enc.convert, enc.encode, Double(statBytes) * 8 / seconds / 1e6))
        }
        statCaptured = 0
        statEncoded = 0
        statDropped = 0
        statHandlerMicros = 0
        statPipelineMicros = 0
        statBytes = 0
        statSince = now
    }

    private(set) var displayID: CGDirectDisplayID = CGMainDisplayID()
    /// 输出像素 / 显示器原生像素，用于按比例缩放鼠标指针。
    var onOutputScale: ((Double) -> Void)?

    init(maxFps: Int, bitrateMbps: Int, maxQP: Int, losslessStatic: Bool) {
        self.maxFps = maxFps
        self.bitrateMbps = bitrateMbps
        self.maxQP = maxQP
        losslessEnabled = losslessStatic
        super.init()
        CGDisplayRegisterReconfigurationCallback({ _, flags, userInfo in
            guard let userInfo, !flags.contains(.beginConfigurationFlag) else { return }
            Unmanaged<ScreenCapture>.fromOpaque(userInfo).takeUnretainedValue().displaysChanged()
        }, Unmanaged.passUnretained(self).toOpaque())
    }

    /// 开始输出。viewWidth/viewHeight 为客户端可用显示区域（物理像素），0 表示不限制。
    /// isDrained(bytes)：网络积压是否低于给定字节数，用来决定何时插入下一批无损数据。
    /// canSendVideo：网络是否能接收下一帧视频（采集端限流用）。
    func start(viewWidth: Int, viewHeight: Int, video: @escaping VideoSink, lossless: @escaping LosslessSink,
               isDrained: @escaping (Int) -> Bool, canSendVideo: @escaping () -> Bool) {
        let threshold = losslessDrainedBytes
        losslessQueue.async { [self] in
            losslessReady = { isDrained(threshold) }
        }
        queue.async { [self] in
            self.canSendVideo = canSendVideo
            videoSink = video
            losslessSink = lossless
            generation += 1
            markAllDirty()
            if viewWidth != self.viewWidth || viewHeight != self.viewHeight || stream == nil {
                self.viewWidth = viewWidth
                self.viewHeight = viewHeight
                restartStream()
                return
            }
            encoder?.requestKeyframe()
            if let lastFrame {
                encode(lastFrame, captureMicros: Clock.nowMicros(), refine: false)
                scheduleStatic(serial: frameSerial)
            }
        }
    }

    /// 客户端窗口大小变化。
    func updateView(width: Int, height: Int) {
        queue.async { [self] in
            guard videoSink != nil, width != viewWidth || height != viewHeight else { return }
            viewWidth = width
            viewHeight = height
            Log.info("客户端显示区域变为 \(width)×\(height)")
            restartStream()
        }
    }

    func stop() {
        queue.async { [self] in
            videoSink = nil
            losslessSink = nil
            generation += 1
            stopStream()
            Log.info("已停止采集")
        }
    }

    /// 客户端请求关键帧（解码出错或刚连上）。画面静止时立即用最后一帧重新编码。
    func requestKeyframe() {
        queue.async { [self] in
            encoder?.requestKeyframe()
            if let lastFrame {
                encode(lastFrame, captureMicros: Clock.nowMicros(), refine: false)
            }
        }
    }

    // MARK: - 采集流

    private func displaysChanged() {
        queue.async { [self] in
            guard videoSink != nil, !reconfigurePending else { return }
            reconfigurePending = true
            // 显示器变化时会连续触发多次回调，稍等合并成一次重启。
            queue.asyncAfter(deadline: .now() + 0.8) { [self] in
                reconfigurePending = false
                guard videoSink != nil else { return }
                Log.info("显示器配置变化，重新采集")
                restartStream()
            }
        }
    }

    private func restartStream() {
        stopStream()
        generation += 1
        startStream(generation)
    }

    private func stopStream() {
        let old = stream
        stream = nil
        encoder = nil
        lastFrame = nil
        outbox.removeAll()
        droppedDirty = nil
        deferredFrame = nil
        bumpSerial()
        old?.stopCapture { error in
            if let error {
                Log.info("停止采集：\(error.localizedDescription)")
            }
        }
    }

    private func startStream(_ expectedGeneration: Int) {
        SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: true) { [self] content, error in
            queue.async { [self] in
                guard expectedGeneration == generation, videoSink != nil, stream == nil else { return }
                let mainID = CGMainDisplayID()
                guard let content, let display = content.displays.first(where: { $0.displayID == mainID }) ?? content.displays.first else {
                    Log.info("无法获取屏幕：\(error?.localizedDescription ?? "没有可用屏幕")。请在“系统设置 → 隐私与安全性 → 录屏与系统录音”里允许 MacStream，然后重新打开它")
                    return
                }
                displayID = display.displayID
                let mode = CGDisplayCopyDisplayMode(display.displayID)
                let nativeWidth = mode?.pixelWidth ?? display.width
                let nativeHeight = mode?.pixelHeight ?? display.height
                let refresh = Int((mode?.refreshRate ?? 0).rounded())
                currentFps = min(maxFps, refresh > 0 ? refresh : 60)

                // 适配客户端显示区域（只缩小不放大），保持宽高比。
                // 宽高都取 16 的倍数（编码宏块对齐）：否则编码器要在边缘补填充行，
                // 客户端还原色度时会渗入填充数据，窗口模式下底边出现绿条。
                var scale = 1.0
                if viewWidth > 0 && viewHeight > 0 {
                    scale = min(1, Double(viewWidth) / Double(nativeWidth), Double(viewHeight) / Double(nativeHeight))
                }
                let width = max(16, Int((Double(nativeWidth) * scale / 16).rounded(.down)) * 16)
                let height = max(16, Int((Double(nativeHeight) * scale / 16).rounded(.down)) * 16)
                onOutputScale?(Double(width) / Double(nativeWidth))

                let configuration = SCStreamConfiguration()
                configuration.width = width
                configuration.height = height
                // 最小间隔若恰好等于刷新周期，刷新时刻的微小抖动会让 ScreenCaptureKit 误丢帧（掉到 30–45 fps）。
                // 放宽到刷新周期的 1/4，实际帧率仍由屏幕刷新决定。
                configuration.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(currentFps * 4))
                configuration.pixelFormat = kCVPixelFormatType_32BGRA
                configuration.colorSpaceName = CGColorSpace.sRGB
                configuration.showsCursor = false
                configuration.queueDepth = 8

                let filter = SCContentFilter(display: display, excludingWindows: [])
                let newStream = SCStream(filter: filter, configuration: configuration, delegate: self)
                do {
                    try newStream.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
                } catch {
                    Log.info("添加采集输出失败：\(error)")
                    return
                }
                stream = newStream
                let fps = currentFps
                newStream.startCapture { [self] error in
                    if let error {
                        Log.info("启动采集失败：\(error.localizedDescription)")
                        queue.async { [self] in
                            if stream === newStream {
                                stream = nil
                            }
                        }
                    } else {
                        Log.info("开始采集 显示器 \(display.displayID) 原生 \(nativeWidth)×\(nativeHeight) → 输出 \(width)×\(height) @ \(fps) fps，静止无损 \(losslessEnabled ? "开" : "关")")
                    }
                }
            }
        }
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, stream === self.stream, videoSink != nil,
              let infos = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let info = infos.first,
              let rawStatus = info[.status] as? Int,
              SCFrameStatus(rawValue: rawStatus) == .complete,
              let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer)
        else { return }

        let handlerStart = Clock.nowMicros()
        let captureMicros = (info[.displayTime] as? UInt64).map(Clock.micros(machTicks:)) ?? handlerStart
        statCaptured += 1
        deferredFrame = (pixelBuffer, captureMicros)
        pump()
        statHandlerMicros += Clock.nowMicros() - handlerStart
        reportStats()
    }

    /// 处理最新一帧，前提是：(1) 未超过自适应帧率上限；(2) 网络积压已足够少。
    /// 否则只保留最新一帧，稍后再试——跳过的帧不编码（下一帧与上一编码帧比对，变化不会丢），
    /// 编码器也就不会因为丢帧而需要补关键帧；画面停下时最新内容一定会送达。
    private func pump() {
        guard let (buffer, micros) = deferredFrame, videoSink != nil else { return }
        let now = Clock.nowMicros()
        var wait = 0.0
        if rateFps < currentFps {
            let interval = UInt64(1_000_000 / max(rateFps, 1))
            let elapsed = now &- lastProcessed
            if elapsed < interval * 9 / 10 {
                wait = Double(interval - elapsed) / 1e6
            }
        }
        if wait == 0 && !canSendVideo() {
            wait = 0.004
        }
        if wait > 0 {
            if !pumpScheduled {
                pumpScheduled = true
                queue.asyncAfter(deadline: .now() + wait) { [self] in
                    pumpScheduled = false
                    pump()
                }
            }
            return
        }
        deferredFrame = nil
        process(buffer, captureMicros: micros)
    }

    private func process(_ pixelBuffer: CVPixelBuffer, captureMicros: UInt64) {
        lastProcessed = Clock.nowMicros()
        let previous = lastFrame
        lastFrame = pixelBuffer
        let serial = bumpSerial()

        // 编码和图块比对都是异步的，并行进行；两者都完成后该帧才按顺序发出。
        encode(pixelBuffer, captureMicros: captureMicros, refine: false, needsMask: true)
        diffTiles(previous: previous, current: pixelBuffer, serial: serial) { [self] mask in
            if let index = outbox.firstIndex(where: { $0.micros == captureMicros && $0.needsMask && $0.mask == nil }) {
                outbox[index].mask = mask
            }
            flushOutbox()
        }
        scheduleStatic(serial: serial)
    }

    // MARK: - 自适应码率

    private var rateFps = 1000
    private var rateBitrate = Int.max
    private var lastProcessed: UInt64 = 0
    private var deferredFrame: (CVPixelBuffer, UInt64)?
    private var pumpScheduled = false
    private var canSendVideo: () -> Bool = { true }

    /// 由 RateController 调用：码率上限（bps）与帧率上限。
    func applyRate(bitrate: Int, fps: Int) {
        queue.async { [self] in
            rateBitrate = bitrate
            rateFps = fps
            if let encoder {
                encoder.setBitrate(effectiveBitrate(width: encoder.width, height: encoder.height),
                                   fps: min(currentFps, rateFps))
            }
        }
    }

    private func effectiveBitrate(width: Int, height: Int) -> Int {
        min(bitrateFor(width: width, height: height, fps: min(currentFps, rateFps)), rateBitrate)
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        Log.info("采集中断：\(error.localizedDescription)")
        queue.async { [self] in
            guard self.stream === stream else { return }
            self.stream = nil
            let expected = generation
            queue.asyncAfter(deadline: .now() + 1) { [self] in
                if expected == generation, videoSink != nil, self.stream == nil {
                    startStream(expected)
                }
            }
        }
    }

    // MARK: - 图块比对

    @discardableResult
    private func bumpSerial() -> UInt32 {
        frameSerial &+= 1
        serialLock.lock()
        latestSerial = frameSerial
        serialLock.unlock()
        return frameSerial
    }

    private func currentSerial() -> UInt32 {
        serialLock.lock()
        defer { serialLock.unlock() }
        return latestSerial
    }

    private func markAllDirty() {
        for i in dirtySinceLossless.indices {
            dirtySinceLossless[i] = true
        }
    }

    /// 找出两帧之间内容变化的图块。优先在 GPU 上异步比较（不占用采集线程），否则在 CPU 上逐行比对。
    /// completion 总是在采集队列上调用。
    private func diffTiles(previous: CVPixelBuffer?, current: CVPixelBuffer, serial: UInt32,
                           completion: @escaping (TileMask) -> Void) {
        let width = CVPixelBufferGetWidth(current)
        let height = CVPixelBufferGetHeight(current)
        let tile = Self.tileSize
        let tx = (width + tile - 1) / tile
        let ty = (height + tile - 1) / tile
        if tx != tilesX || ty != tilesY {
            tilesX = tx
            tilesY = ty
            dirtySinceLossless = [Bool](repeating: true, count: tx * ty)
        }
        let sameSize = previous.map { CVPixelBufferGetWidth($0) == width && CVPixelBufferGetHeight($0) == height } ?? false

        if sameSize, let previous, let gpuDiffer,
           gpuDiffer.diff(previous, current, tileSize: tile, tilesX: tx, tilesY: ty, completion: { [self] changed in
               queue.async { [self] in
                   completion(makeMask(changed ?? [UInt8](repeating: 1, count: tx * ty), tilesX: tx, tilesY: ty, serial: serial))
               }
           }) {
            return
        }
        completion(makeMask(cpuDiff(previous: sameSize ? previous : nil, current: current, tilesX: tx, tilesY: ty),
                            tilesX: tx, tilesY: ty, serial: serial))
    }

    private func makeMask(_ changed: [UInt8], tilesX tx: Int, tilesY ty: Int, serial: UInt32) -> TileMask {
        // 比对期间网格变了（画面尺寸变化）：全部视为变化。
        let sameGrid = tx == tilesX && ty == tilesY
        var bits = [UInt8](repeating: 0, count: (tx * ty + 7) / 8)
        for i in 0..<(tx * ty) where changed[i] != 0 || !sameGrid {
            bits[i >> 3] |= 1 << (i & 7)
            if sameGrid {
                dirtySinceLossless[i] = true
            }
        }
        return TileMask(serial: serial, tilesX: tx, tilesY: ty, bits: bits)
    }

    /// CPU 逐行比较（行相同直接跳过，按图块行并行）。previous 为 nil 时全部视为变化。
    private func cpuDiff(previous: CVPixelBuffer?, current: CVPixelBuffer, tilesX tx: Int, tilesY ty: Int) -> [UInt8] {
        let width = CVPixelBufferGetWidth(current)
        let height = CVPixelBufferGetHeight(current)
        let tile = Self.tileSize
        var changed = [UInt8](repeating: 0, count: tx * ty)
        if let previous {
            CVPixelBufferLockBaseAddress(previous, .readOnly)
            CVPixelBufferLockBaseAddress(current, .readOnly)
            if let prevBase = CVPixelBufferGetBaseAddress(previous), let curBase = CVPixelBufferGetBaseAddress(current) {
                let prevStride = CVPixelBufferGetBytesPerRow(previous)
                let curStride = CVPixelBufferGetBytesPerRow(current)
                changed.withUnsafeMutableBufferPointer { flags in
                    let flagBase = flags.baseAddress!
                    DispatchQueue.concurrentPerform(iterations: ty) { row in
                        let y0 = row * tile
                        let y1 = min(height, y0 + tile)
                        for y in y0..<y1 {
                            let p = prevBase + y * prevStride
                            let c = curBase + y * curStride
                            if memcmp(p, c, width * 4) == 0 { continue }
                            for col in 0..<tx where flagBase[row * tx + col] == 0 {
                                let offset = col * tile * 4
                                let length = min(tile, width - col * tile) * 4
                                if memcmp(p + offset, c + offset, length) != 0 {
                                    flagBase[row * tx + col] = 1
                                }
                            }
                        }
                    }
                }
            } else {
                changed = [UInt8](repeating: 1, count: tx * ty)
            }
            CVPixelBufferUnlockBaseAddress(current, .readOnly)
            CVPixelBufferUnlockBaseAddress(previous, .readOnly)
        } else {
            changed = [UInt8](repeating: 1, count: tx * ty)
        }
        return changed
    }

    // MARK: - 静止处理（无损 / 补清晰）

    private func scheduleStatic(serial: UInt32) {
        if losslessEnabled {
            queue.asyncAfter(deadline: .now() + losslessDelay) { [self] in
                guard serial == frameSerial else { return }
                sendLossless(serial: serial)
            }
        } else {
            scheduleRefine(serial: serial, step: 0, delay: refineDelay)
        }
    }

    /// 画面静止后，用最后一帧再编码几次：同一画面的后续 P 帧会逐步把量化损失补回来。
    private func scheduleRefine(serial: UInt32, step: Int, delay: Double) {
        guard step < refineFrames else { return }
        queue.asyncAfter(deadline: .now() + delay) { [self] in
            guard serial == frameSerial, videoSink != nil, let lastFrame else { return }
            encode(lastFrame, captureMicros: Clock.nowMicros(), refine: true)
            scheduleRefine(serial: serial, step: step + 1, delay: 1.0 / Double(currentFps))
        }
    }

    /// 把静止画面中变化过的图块无损发送。分批写入，每批写完才压缩下一批；画面一变就中止，绝不堵住视频。
    private func sendLossless(serial: UInt32) {
        guard !losslessInFlight, let frame = lastFrame, let sink = losslessSink else { return }
        let tiles = dirtySinceLossless.indices.filter { dirtySinceLossless[$0] }
        guard !tiles.isEmpty else { return }
        for i in tiles {
            dirtySinceLossless[i] = false
        }
        losslessInFlight = true
        let tilesX = tilesX
        let batchLimit = losslessBatchBytes

        losslessQueue.async { [self] in
            let started = Clock.nowMicros()
            var unsent: [Int] = []
            var sentTiles = 0
            var sentBytes = 0
            CVPixelBufferLockBaseAddress(frame, .readOnly)
            if let base = CVPixelBufferGetBaseAddress(frame) {
                let width = CVPixelBufferGetWidth(frame)
                let height = CVPixelBufferGetHeight(frame)
                let stride = CVPixelBufferGetBytesPerRow(frame)
                let tile = Self.tileSize
                var raw = [UInt8](repeating: 0, count: tile * tile * 4)
                var packed = [UInt8](repeating: 0, count: tile * tile * 4 + 1024)
                let scratch = UnsafeMutableRawPointer.allocate(
                    byteCount: compression_encode_scratch_buffer_size(COMPRESSION_LZ4_RAW), alignment: 16)
                defer { scratch.deallocate() }

                var body = ByteWriter(capacity: batchLimit + 64 * 1024)
                var count = 0
                // 客户端先暂存各批图块，收到 final 批后才整体生效，避免一批批生效露出阶梯。
                let flush: (_ final: Bool) -> Bool = { final in
                    guard count > 0 || final else { return true }
                    var header = ByteWriter(capacity: 16 + body.bytes.count)
                    header.u32(serial)
                    header.u16(UInt16(width))
                    header.u16(UInt16(height))
                    header.u16(UInt16(tile))
                    header.u16(UInt16(count))
                    header.u8(final ? 1 : 0)
                    header.append(body.bytes)
                    // 等网络积压清空再发（最多等 3 秒）；期间画面一变就放弃这一轮。
                    let waitUntil = Clock.nowMicros() + 3_000_000
                    while !self.losslessReady() {
                        if self.currentSerial() != serial || Clock.nowMicros() > waitUntil {
                            return false
                        }
                        usleep(2000)
                    }
                    let semaphore = DispatchSemaphore(value: 0)
                    let accepted = sink(header.bytes) { semaphore.signal() }
                    if accepted {
                        semaphore.wait()
                    }
                    sentBytes += header.bytes.count
                    body = ByteWriter(capacity: batchLimit + 64 * 1024)
                    count = 0
                    return accepted
                }

                var index = 0
                while index < tiles.count {
                    if currentSerial() != serial {
                        break
                    }
                    let t = tiles[index]
                    let x0 = (t % tilesX) * tile
                    let y0 = (t / tilesX) * tile
                    let w = min(tile, width - x0)
                    let h = min(tile, height - y0)
                    let rowBytes = w * 4
                    raw.withUnsafeMutableBytes { dst in
                        for y in 0..<h {
                            memcpy(dst.baseAddress! + y * rowBytes, base + (y0 + y) * stride + x0 * 4, rowBytes)
                        }
                    }
                    let rawCount = rowBytes * h
                    let packedCount = raw.withUnsafeBytes { src in
                        packed.withUnsafeMutableBytes { dst in
                            compression_encode_buffer(dst.bindMemory(to: UInt8.self).baseAddress!, dst.count,
                                                      src.bindMemory(to: UInt8.self).baseAddress!, rawCount,
                                                      scratch, COMPRESSION_LZ4_RAW)
                        }
                    }
                    body.u16(UInt16(t % tilesX))
                    body.u16(UInt16(t / tilesX))
                    if packedCount > 0 && packedCount < rawCount {
                        body.u32(UInt32(packedCount))
                        body.append(Array(packed[0..<packedCount]))
                    } else {
                        body.u32(UInt32(rawCount) | 0x8000_0000)
                        body.append(Array(raw[0..<rawCount]))
                    }
                    count += 1
                    sentTiles += 1
                    index += 1
                    if body.bytes.count >= batchLimit {
                        if !flush(false) { break }
                    }
                }
                let complete = index == tiles.count && currentSerial() == serial && flush(true)
                // 中止时客户端不会让这一轮生效，整轮图块重新标记为待发送。
                if !complete {
                    unsent = tiles
                    sentTiles = 0
                }
            }
            CVPixelBufferUnlockBaseAddress(frame, .readOnly)

            let elapsed = (Clock.nowMicros() - started) / 1000
            queue.async { [self] in
                losslessInFlight = false
                for t in unsent where t < dirtySinceLossless.count {
                    dirtySinceLossless[t] = true
                }
                if sentTiles > 0 {
                    Log.info("无损图块 \(sentTiles)/\(tiles.count) 块，\(sentBytes / 1024) KB，\(elapsed) ms\(unsent.isEmpty ? "" : "（画面变化，已中止）")")
                }
            }
        }
    }

    // MARK: - 编码

    /// needsMask：这是一帧新采集的画面，要等它的图块比对结果一起发出。
    private func encode(_ pixelBuffer: CVPixelBuffer, captureMicros: UInt64, refine: Bool, needsMask: Bool = false) {
        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)
        if encoder == nil || encoder!.width != width || encoder!.height != height {
            let bitrate = effectiveBitrate(width: width, height: height)
            do {
                encoder = try VideoEncoder(width: width, height: height, fps: min(currentFps, rateFps), bitrate: bitrate,
                                           maxQP: maxQP) { [weak self] data, key, micros, refine in
                    self?.deliver(data, key, refine, micros)
                }
            } catch {
                Log.info("创建编码器失败：\(error)")
                return
            }
        }
        // 色彩转换 + 提交编码放到独立串行队列，与采集回调里的图块比对并行，不占用采集线程。
        // 编码器内部是“转换 → 硬件编码”两级异步流水线，不占用采集线程。
        outbox.append(Outgoing(micros: captureMicros, created: Clock.nowMicros(), needsMask: needsMask))
        encoder!.encode(pixelBuffer, captureMicros: captureMicros, refine: refine)
    }

    /// bitrateMbps > 0 时固定码率；为 0 时按像素速率自动：1080p60 ≈ 120 Mbps，1440p60 ≈ 185 Mbps，上限 250 Mbps。
    private func bitrateFor(width: Int, height: Int, fps: Int) -> Int {
        if bitrateMbps > 0 {
            return bitrateMbps * 1_000_000
        }
        let ratio = Double(width * height * fps) / Double(1920 * 1080 * 60)
        let mbps = min(250, max(40, 120 * pow(ratio, 0.75)))
        return Int(mbps * 1_000_000)
    }

    /// 编码器回调线程 → 采集队列：填入待发队列，然后按顺序发出已就绪的帧。
    private func deliver(_ data: [UInt8], _ key: Bool, _ refine: Bool, _ micros: UInt64) {
        queue.async { [self] in
            guard let index = outbox.firstIndex(where: { $0.micros == micros && $0.video == nil }) else { return }
            outbox[index].video = EncodedVideo(data: data, keyframe: key, refine: refine, captureMicros: micros, dirty: nil)
            flushOutbox()
        }
    }

    /// 从队头依次发出“编码完成且图块比对完成”的帧，保证解码顺序。
    /// 卡住超过 250 ms 的条目（编码失败等）直接丢弃，它的变化图块并入下一帧。
    private func flushOutbox() {
        let now = Clock.nowMicros()
        while let first = outbox.first {
            let ready = first.video != nil && (!first.needsMask || first.mask != nil)
            if !ready {
                guard now &- first.created > 250_000 else { break }
                outbox.removeFirst()
                if let mask = first.mask {
                    droppedDirty = Self.union(droppedDirty, mask)
                } else if first.needsMask {
                    droppedDirty = allDirtyMask()
                }
                encoder?.requestKeyframe()
                continue
            }
            outbox.removeFirst()
            send(first.video!, mask: first.mask)
        }
    }

    /// 被丢弃的帧的变化图块会并入下一帧，保证客户端不会把旧的无损像素盖在新内容上。
    private func send(_ video: EncodedVideo, mask: TileMask?) {
        var mask = mask
        if let carried = droppedDirty {
            mask = Self.union(mask, carried)
        }
        guard let videoSink else { return }
        let frame = EncodedVideo(data: video.data, keyframe: video.keyframe, refine: video.refine,
                                 captureMicros: video.captureMicros, dirty: mask)
        if videoSink(frame) {
            droppedDirty = nil
            if !video.refine {
                statEncoded += 1
                statBytes += video.data.count
                // displayTime 可能略晚于当前时刻（计划上屏时间），差值为负时按 0 计。
                let elapsed = Int64(bitPattern: Clock.nowMicros() &- video.captureMicros)
                statPipelineMicros += UInt64(max(0, elapsed))
            }
        } else {
            droppedDirty = mask
            statDropped += 1
            encoder?.requestKeyframe()
        }
    }

    private func allDirtyMask() -> TileMask {
        TileMask(serial: frameSerial, tilesX: tilesX, tilesY: tilesY,
                 bits: [UInt8](repeating: 0xFF, count: (tilesX * tilesY + 7) / 8))
    }

    private var droppedDirty: TileMask?

    private static func union(_ a: TileMask?, _ b: TileMask) -> TileMask {
        guard let a else { return b }
        guard a.tilesX == b.tilesX, a.tilesY == b.tilesY else {
            // 图块网格不同（尺寸刚变化）：全部视为变化。
            return TileMask(serial: max(a.serial, b.serial), tilesX: a.tilesX, tilesY: a.tilesY,
                            bits: [UInt8](repeating: 0xFF, count: a.bits.count))
        }
        return TileMask(serial: max(a.serial, b.serial), tilesX: a.tilesX, tilesY: a.tilesY,
                        bits: zip(a.bits, b.bits).map { $0 | $1 })
    }
}
