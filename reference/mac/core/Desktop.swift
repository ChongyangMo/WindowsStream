import AppKit
import IOKit.pwr_mgt

/// 轮询系统当前鼠标指针形状，变化时把位图发给客户端，由客户端本地绘制（指针零延迟）。
final class CursorMonitor {
    private let onChange: ([UInt8]) -> Void
    private var timer: Timer?
    private var lastSignature: Int?
    /// 串流输出像素 / 显示器原生像素；指针按同样比例缩放，和画面大小匹配。只在主线程访问。
    private var streamScale = 1.0

    init(onChange: @escaping ([UInt8]) -> Void) {
        self.onChange = onChange
    }

    func setStreamScale(_ scale: Double) {
        guard scale > 0, abs(scale - streamScale) > 0.001 else { return }
        streamScale = scale
        lastSignature = nil
        if timer != nil {
            poll()
        }
    }

    func start() {
        DispatchQueue.main.async { [self] in
            timer?.invalidate()
            lastSignature = nil
            timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30, repeats: true) { [weak self] _ in self?.poll() }
            poll()
        }
    }

    func stop() {
        DispatchQueue.main.async { [self] in
            timer?.invalidate()
            timer = nil
        }
    }

    private func poll() {
        guard let cursor = NSCursor.currentSystem else { return }
        let image = cursor.image
        let scale = (NSScreen.main?.backingScaleFactor ?? 1) * streamScale
        let width = max(1, Int((image.size.width * scale).rounded()))
        let height = max(1, Int((image.size.height * scale).rounded()))
        guard width <= 256, height <= 256,
              let space = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                                      space: space,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return }

        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(cgContext: context, flipped: false)
        image.draw(in: NSRect(x: 0, y: 0, width: width, height: height))
        NSGraphicsContext.restoreGraphicsState()
        guard let data = context.data else { return }

        let raw = UnsafeRawBufferPointer(start: data, count: width * height * 4)
        let hotX = Int((cursor.hotSpot.x * scale).rounded())
        let hotY = Int((cursor.hotSpot.y * scale).rounded())
        var hasher = Hasher()
        hasher.combine(width)
        hasher.combine(height)
        hasher.combine(hotX)
        hasher.combine(hotY)
        hasher.combine(bytes: raw)
        let signature = hasher.finalize()
        guard signature != lastSignature else { return }
        lastSignature = signature

        var writer = ByteWriter(capacity: 8 + raw.count)
        writer.u16(UInt16(width))
        writer.u16(UInt16(height))
        writer.u16(UInt16(clamping: hotX))
        writer.u16(UInt16(clamping: hotY))
        writer.append(Array(raw))
        onChange(writer.bytes)
    }
}

/// 双向剪贴板同步（UTF-8 文本，支持中文）。
final class ClipboardSync {
    private let onLocalChange: (String) -> Void
    private var timer: Timer?
    private var changeCount = 0
    private var lastText: String?

    init(onLocalChange: @escaping (String) -> Void) {
        self.onLocalChange = onLocalChange
    }

    func start() {
        DispatchQueue.main.async { [self] in
            timer?.invalidate()
            changeCount = NSPasteboard.general.changeCount
            timer = Timer.scheduledTimer(withTimeInterval: 0.3, repeats: true) { [weak self] _ in self?.poll() }
        }
    }

    func stop() {
        DispatchQueue.main.async { [self] in
            timer?.invalidate()
            timer = nil
        }
    }

    func applyRemote(_ text: String) {
        DispatchQueue.main.async { [self] in
            guard text != lastText else { return }
            lastText = text
            let pasteboard = NSPasteboard.general
            pasteboard.clearContents()
            pasteboard.setString(text, forType: .string)
            changeCount = pasteboard.changeCount
        }
    }

    private func poll() {
        let pasteboard = NSPasteboard.general
        guard pasteboard.changeCount != changeCount else { return }
        changeCount = pasteboard.changeCount
        guard let text = pasteboard.string(forType: .string), text != lastText, text.utf8.count <= 4 * 1024 * 1024 else { return }
        lastText = text
        onLocalChange(text)
    }
}

/// 有客户端连接时阻止显示器休眠，并在连接时唤醒屏幕。
final class DisplayKeeper {
    private var assertion: IOPMAssertionID = 0
    private var active = false

    func begin() {
        var activity: IOPMAssertionID = 0
        IOPMAssertionDeclareUserActivity("MacStream 客户端连接" as CFString, kIOPMUserActiveLocal, &activity)
        guard !active else { return }
        active = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep as CFString,
                                             IOPMAssertionLevel(kIOPMAssertionLevelOn),
                                             "MacStream 客户端已连接" as CFString, &assertion) == kIOReturnSuccess
    }

    func end() {
        guard active else { return }
        IOPMAssertionRelease(assertion)
        active = false
    }
}
