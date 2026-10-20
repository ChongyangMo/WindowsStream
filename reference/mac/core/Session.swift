import CoreGraphics
import CryptoKit
import Foundation

/// 客户端在握手时告知的显示信息（物理像素）。旧客户端未提供的字段为 0。
struct ClientDisplay {
    var viewWidth = 0
    var viewHeight = 0
    var monitorWidth = 0
    var monitorHeight = 0
    var refresh = 0
    var wireless = false
}

/// 管理当前客户端：认证、串流开关、输入分发。新客户端连上时会顶替旧连接。
final class Session {
    private let virtualDisplay = VirtualDisplayManager()
    private let config: Config
    private let capture: ScreenCapture
    private let injector: InputInjector
    private let inputQueue = DispatchQueue(label: "macstream.input", qos: .userInteractive)
    private let keeper = DisplayKeeper()
    private let lock = NSLock()
    private var current: Connection?
    private var cursor: CursorMonitor!
    private var clipboard: ClipboardSync!

    init(config: Config) {
        self.config = config
        let capture = ScreenCapture(maxFps: config.fps, bitrateMbps: config.bitrateMbps, maxQP: config.maxQP,
                                    losslessStatic: config.losslessStatic)
        self.capture = capture
        injector = InputInjector(displayID: { capture.displayID })
        cursor = CursorMonitor { [weak self] payload in self?.activeConnection()?.send(.cursor, payload) }
        clipboard = ClipboardSync { [weak self] text in self?.activeConnection()?.send(.clipboard, Array(text.utf8)) }
        capture.onOutputScale = { [weak self] scale in
            DispatchQueue.main.async { self?.cursor.setStreamScale(scale) }
        }
    }

    func accept(_ connection: Connection) {
        let thread = Thread { [self] in serve(connection) }
        thread.name = "macstream.client"
        thread.start()
    }

    private func activeConnection() -> Connection? {
        lock.lock()
        defer { lock.unlock() }
        return current
    }

    private func serve(_ connection: Connection) {
        defer { connection.release() }
        let client: ClientDisplay
        do {
            client = try handshake(connection)
        } catch {
            Log.info("\(connection.peer) 认证失败：\(error)")
            return
        }
        Log.info("\(connection.peer) 已连接（\(client.wireless ? "无线" : "有线")），客户端显示区域 \(client.viewWidth)×\(client.viewHeight)，" +
                 "显示器 \(client.monitorWidth)×\(client.monitorHeight) @ \(client.refresh) Hz")

        lock.lock()
        let previous = current
        current = connection
        lock.unlock()
        previous?.close()
        if config.virtualDisplay && client.refresh > 0 {
            // 虚拟屏成为主屏后，采集会通过显示器变化回调自动切换过去。
            DispatchQueue.main.async { [self] in
                virtualDisplay.ensure(width: client.monitorWidth, height: client.monitorHeight, refresh: client.refresh)
            }
        }
        startStreaming(to: connection, maxWidth: client.viewWidth, maxHeight: client.viewHeight)
        let rate = RateController(connection: connection, displayFps: 1000, wireless: client.wireless,
                                  apply: { [capture] target in capture.applyRate(bitrate: target.bitrate, fps: target.fps) },
                                  report: { [weak connection] target, queueMs, linkMbps in
                                      var writer = ByteWriter()
                                      writer.u32(UInt32(target.bitrate / 1000))
                                      writer.u16(UInt16(min(target.fps, 65535)))
                                      writer.u16(UInt16(min(queueMs, 65535)))
                                      writer.u32(UInt32(linkMbps * 1000))
                                      connection?.send(.stats, writer.bytes)
                                  })
        rate.start()
        defer { rate.stop() }

        do {
            while true {
                let (type, payload) = try connection.readMessage()
                try handle(type, payload, connection)
            }
        } catch {
            Log.info("\(connection.peer) 已断开：\(error)")
        }

        inputQueue.async { [injector] in injector.releaseAll() }
        lock.lock()
        let wasCurrent = current === connection
        if wasCurrent {
            current = nil
        }
        lock.unlock()
        if wasCurrent {
            stopStreaming()
        }
    }

    private func handshake(_ connection: Connection) throws -> ClientDisplay {
        connection.setReadTimeout(seconds: 5)
        var nonce = [UInt8](repeating: 0, count: 32)
        _ = SecRandomCopyBytes(kSecRandomDefault, nonce.count, &nonce)
        var hello = ByteWriter()
        hello.u32(protocolVersion)
        hello.append(nonce)
        connection.send(.serverHello, hello.bytes)

        let (type, payload) = try connection.readMessage()
        guard type == MessageType.clientAuth.rawValue else { throw ProtocolError.unexpected(type) }
        var reader = ByteReader(payload)
        let mac = try reader.take(32)
        let version = try reader.u32()
        var client = ClientDisplay()
        if reader.remaining >= 4 {
            client.viewWidth = Int(try reader.u16())
            client.viewHeight = Int(try reader.u16())
        }
        if reader.remaining >= 6 {
            client.monitorWidth = Int(try reader.u16())
            client.monitorHeight = Int(try reader.u16())
            client.refresh = Int(try reader.u16())
        }
        if reader.remaining >= 1 {
            client.wireless = try reader.u8() != 0
        }
        let key = SymmetricKey(data: Array(config.token.utf8))
        guard HMAC<SHA256>.isValidAuthenticationCode(mac, authenticating: nonce, using: key), version == protocolVersion else {
            connection.send(.authResult, [0])
            throw ProtocolError.authFailed
        }

        let mode = CGDisplayCopyDisplayMode(capture.displayID)
        var result = ByteWriter()
        result.u8(1)
        result.u16(UInt16(clamping: mode?.pixelWidth ?? 0))
        result.u16(UInt16(clamping: mode?.pixelHeight ?? 0))
        result.u16(UInt16(clamping: config.fps))
        connection.send(.authResult, result.bytes)
        connection.setReadTimeout(seconds: 0)
        return client
    }

    private func startStreaming(to connection: Connection, maxWidth: Int, maxHeight: Int) {
        DispatchQueue.main.async { [self] in
            keeper.begin()
            cursor.start()
            clipboard.start()
        }
        capture.start(viewWidth: maxWidth, viewHeight: maxHeight, video: { [weak connection] frame in
            guard let connection, !connection.isClosed else { return false }
            var writer = ByteWriter(capacity: frame.data.count + 32 + (frame.dirty?.bits.count ?? 0))
            writer.u64(frame.captureMicros)
            writer.u8((frame.keyframe ? 1 : 0) | (frame.refine ? 2 : 0) | (frame.dirty != nil ? 4 : 0))
            if let dirty = frame.dirty {
                writer.u32(dirty.serial)
                writer.u16(UInt16(ScreenCapture.tileSize))
                writer.u16(UInt16(dirty.tilesX))
                writer.u16(UInt16(dirty.tilesY))
                writer.append(dirty.bits)
            }
            writer.append(frame.data)
            return connection.send(.video, writer.bytes, droppableVideo: true, isVideo: true)
        }, lossless: { [weak connection] payload, done in
            guard let connection, !connection.isClosed else { return false }
            return connection.send(.lossless, payload, completion: done)
        }, isDrained: { [weak connection] bytes in
            connection?.isDrained(below: bytes) ?? true
        }, canSendVideo: { [weak connection] in
            // 网络积压（含在途数据）低于 384 KB 才编码下一帧：源头限流，不丢已编码的帧，也就不需要补关键帧。
            connection?.canAcceptVideo(belowBytes: 384 * 1024) ?? false
        })
    }

    private func stopStreaming() {
        capture.stop()
        DispatchQueue.main.async { [self] in
            virtualDisplay.releaseLater()
            keeper.end()
            cursor.stop()
            clipboard.stop()
        }
    }

    private func handle(_ type: UInt8, _ payload: [UInt8], _ connection: Connection) throws {
        var reader = ByteReader(payload)
        switch MessageType(rawValue: type) {
        case .ping:
            let clientMicros = try reader.u64()
            var writer = ByteWriter()
            writer.u64(clientMicros)
            writer.u64(Clock.nowMicros())
            connection.send(.pong, writer.bytes)
        case .mouseMove:
            let x = try reader.u16(), y = try reader.u16()
            inputQueue.async { [injector] in injector.move(x: x, y: y) }
        case .mouseButton:
            let button = Int(try reader.u8()), down = try reader.u8() != 0
            let x = try reader.u16(), y = try reader.u16()
            inputQueue.async { [injector] in injector.button(button, down: down, x: x, y: y) }
        case .scroll:
            let dx = try reader.i16(), dy = try reader.i16()
            inputQueue.async { [injector] in injector.scroll(dx: dx, dy: dy) }
        case .key:
            let code = try reader.u16(), down = try reader.u8() != 0
            inputQueue.async { [injector] in injector.key(code, down: down) }
        case .releaseAll:
            inputQueue.async { [injector] in injector.releaseAll() }
        case .viewSize:
            let width = Int(try reader.u16()), height = Int(try reader.u16())
            capture.updateView(width: width, height: height)
        case .monitorMode:
            let width = Int(try reader.u16()), height = Int(try reader.u16()), refresh = Int(try reader.u16())
            Log.info("客户端显示器变为 \(width)×\(height) @ \(refresh) Hz")
            if config.virtualDisplay && refresh > 0 {
                DispatchQueue.main.async { [self] in
                    virtualDisplay.ensure(width: width, height: height, refresh: refresh)
                }
            }
        case .clipboard:
            clipboard.applyRemote(String(decoding: payload, as: UTF8.self))
        case .requestKeyframe:
            capture.requestKeyframe()
        default:
            Log.info("忽略未知消息类型 \(type)")
        }
    }
}
