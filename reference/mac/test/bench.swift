// 压测：在主屏打开一个大窗口，每次屏幕刷新都滚动一大片文字（模拟网页滚动），12 秒后退出。
import AppKit
import QuartzCore

final class ScrollView: NSView {
    private let content = CALayer()
    private var offset: CGFloat = 0
    private var link: CADisplayLink?
    private(set) var ticks = 0

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor.white.cgColor
        let scale = NSScreen.main?.backingScaleFactor ?? 2
        let size = NSSize(width: frame.width, height: frame.height * 3)
        let image = NSImage(size: size, flipped: true) { rect in
            NSColor.white.setFill()
            rect.fill()
            let attributes: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: 15),
                .foregroundColor: NSColor.black,
            ]
            if CommandLine.arguments.contains("heavy") {
                // 重负载：满屏彩色小方块（接近不可压缩），模拟视频或图片密集的网页
                var generator = SystemRandomNumberGenerator()
                for by in stride(from: 0, to: Int(size.height), by: 6) {
                    for bx in stride(from: 0, to: Int(size.width), by: 6) {
                        NSColor(calibratedRed: .random(in: 0...1, using: &generator), green: .random(in: 0...1, using: &generator),
                                blue: .random(in: 0...1, using: &generator), alpha: 1).setFill()
                        NSRect(x: bx, y: by, width: 6, height: 6).fill()
                    }
                }
                return true
            }
            var y: CGFloat = 8
            var line = 0
            while y < size.height {
                let text = "第 \(line) 行  MacStream 压测：模拟网页滚动，The quick brown fox jumps over the lazy dog 0123456789 永和九年岁在癸丑"
                (text as NSString).draw(at: NSPoint(x: 16, y: y), withAttributes: attributes)
                if line % 12 == 0 {
                    NSColor(calibratedHue: CGFloat(line % 360) / 360, saturation: 0.6, brightness: 0.9, alpha: 1).setFill()
                    NSRect(x: 900, y: y, width: 300, height: 18).fill()
                }
                y += 22
                line += 1
            }
            return true
        }
        content.contents = image.layerContents(forContentsScale: scale)
        content.contentsScale = scale
        content.frame = CGRect(origin: .zero, size: size)
        layer?.addSublayer(content)
        layer?.masksToBounds = true
    }

    required init?(coder: NSCoder) { fatalError() }

    func start() {
        let link = displayLink(target: self, selector: #selector(step))
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    // 风暴模式：满屏色块每帧随机变色，编码器无法预测，码率冲到上限（用来测试拥堵控制）。
    private var cells: [CALayer] = []

    func enableStorm() {
        let size: CGFloat = 40
        for y in stride(from: 0, to: bounds.height, by: size) {
            for x in stride(from: 0, to: bounds.width, by: size) {
                let cell = CALayer()
                cell.frame = CGRect(x: x, y: y, width: size, height: size)
                layer?.addSublayer(cell)
                cells.append(cell)
            }
        }
    }

    @objc private func step(_ link: CADisplayLink) {
        ticks += 1
        if !cells.isEmpty {
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            for cell in cells {
                cell.backgroundColor = CGColor(red: .random(in: 0...1), green: .random(in: 0...1), blue: .random(in: 0...1), alpha: 1)
            }
            CATransaction.commit()
            return
        }
        offset = (offset + 6).truncatingRemainder(dividingBy: bounds.height * 2)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        content.frame.origin.y = -offset
        CATransaction.commit()
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let screen = NSScreen.main!.frame
let frame = NSRect(x: screen.minX + screen.width * 0.1, y: screen.minY + screen.height * 0.1,
                   width: screen.width * 0.8, height: screen.height * 0.8)
let window = NSWindow(contentRect: frame, styleMask: [.titled], backing: .buffered, defer: false)
window.title = "MacStream 压测（12 秒后自动关闭）"
window.level = .floating
let view = ScrollView(frame: NSRect(origin: .zero, size: frame.size))
window.contentView = view
window.makeKeyAndOrderFront(nil)
app.activate(ignoringOtherApps: true)
if CommandLine.arguments.contains("storm") {
    view.enableStorm()
}
view.start()

DispatchQueue.main.asyncAfter(deadline: .now() + 12) {
    print("窗口刷新 \(view.ticks) 次，约 \(view.ticks / 12) fps")
    exit(0)
}
app.run()
