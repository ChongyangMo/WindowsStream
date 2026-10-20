// 临时测试：创建 2560×1440 HiDPI @ 100 Hz 虚拟屏，检查系统给出的模式并实测刷新率，20 秒后退出（虚拟屏随之消失）。
import AppKit
import CoreVideo

let app = NSApplication.shared
app.setActivationPolicy(.accessory)

let descriptor = CGVirtualDisplayDescriptor()
descriptor.queue = DispatchQueue.main
descriptor.name = "MacStream 测试屏"
descriptor.maxPixelsWide = 5120
descriptor.maxPixelsHigh = 2880
descriptor.sizeInMillimeters = CGSize(width: 597, height: 336)
descriptor.vendorID = 0x4D53
descriptor.productID = 0x0001
descriptor.serialNum = 0x0001
descriptor.terminationHandler = { _, _ in print("虚拟屏被系统终止") }

guard let display = CGVirtualDisplay(descriptor: descriptor) else {
    print("创建虚拟屏失败")
    exit(1)
}
let settings = CGVirtualDisplaySettings()
settings.hiDPI = 1
settings.modes = [
    CGVirtualDisplayMode(width: 2560, height: 1440, refreshRate: 100),
    CGVirtualDisplayMode(width: 2560, height: 1440, refreshRate: 120),
    CGVirtualDisplayMode(width: 2560, height: 1440, refreshRate: 60),
]
print("applySettings:", display.apply(settings), "displayID:", display.displayID)

func describe(_ id: CGDirectDisplayID) {
    if let mode = CGDisplayCopyDisplayMode(id) {
        print("当前模式 \(mode.width)x\(mode.height) 像素 \(mode.pixelWidth)x\(mode.pixelHeight) @ \(mode.refreshRate) Hz")
    }
    let options = [kCGDisplayShowDuplicateLowResolutionModes: true] as CFDictionary
    for mode in (CGDisplayCopyAllDisplayModes(id, options) as? [CGDisplayMode]) ?? [] {
        print("  可选 \(mode.width)x\(mode.height) 像素 \(mode.pixelWidth)x\(mode.pixelHeight) @ \(mode.refreshRate) Hz")
    }
}

var ticks = 0
var link: CVDisplayLink?

DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
    let id = display.displayID
    describe(id)
    // 切到 100 Hz 的 HiDPI 模式
    let options = [kCGDisplayShowDuplicateLowResolutionModes: true] as CFDictionary
    let modes = (CGDisplayCopyAllDisplayModes(id, options) as? [CGDisplayMode]) ?? []
    if let target = modes.first(where: { $0.width == 2560 && $0.pixelWidth == 5120 && Int($0.refreshRate.rounded()) == 100 }) {
        var config: CGDisplayConfigRef?
        CGBeginDisplayConfiguration(&config)
        CGConfigureDisplayWithDisplayMode(config, id, target, nil)
        print("切换到 100 Hz HiDPI：", CGCompleteDisplayConfiguration(config, .forSession) == .success)
    } else {
        print("没有 2560x1440 HiDPI @100 的模式")
    }
    // 让其他显示器（诱骗器）镜像虚拟屏，虚拟屏成为主屏
    var count: UInt32 = 0
    CGGetOnlineDisplayList(0, nil, &count)
    var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
    CGGetOnlineDisplayList(count, &ids, &count)
    var config: CGDisplayConfigRef?
    CGBeginDisplayConfiguration(&config)
    for other in ids where other != id {
        CGConfigureDisplayMirrorOfDisplay(config, other, id)
        print("设置显示器 \(other) 镜像虚拟屏 \(id)")
    }
    print("镜像配置：", CGCompleteDisplayConfiguration(config, .forSession) == .success)
    DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
        print("主屏：\(CGMainDisplayID())，虚拟屏 \(id)，诱骗器镜像于 \(ids.filter { $0 != id }.map { CGDisplayMirrorsDisplay($0) })")
        describe(id)
        CVDisplayLinkCreateWithCGDisplay(id, &link)
        CVDisplayLinkSetOutputCallback(link!, { _, _, _, _, _, _ in
            ticks += 1
            return kCVReturnSuccess
        }, nil)
        CVDisplayLinkStart(link!)
        let start = Date()
        DispatchQueue.main.asyncAfter(deadline: .now() + 5) {
            let seconds = Date().timeIntervalSince(start)
            print(String(format: "CVDisplayLink 实测 %.1f Hz（5 秒内 %d 次）", Double(ticks) / seconds, ticks))
            CVDisplayLinkStop(link!)
        }
    }
}

DispatchQueue.main.asyncAfter(deadline: .now() + 22) {
    print("测试结束，退出")
    exit(0)
}
app.run()
