import AppKit
import CoreGraphics

/// 按客户端显示器创建匹配的 HiDPI 虚拟屏（私有接口 CGVirtualDisplay），设为主屏，其他显示器（诱骗器）镜像它。
/// 刷新率与客户端一致（例如 100 Hz），画面采集随之达到同样帧率。
/// 客户端断开一段时间后释放虚拟屏（进程退出、崩溃时系统也会自动移除），镜像解除，显示器恢复原状。
/// 与鹰锤互联（iPad 投屏）互斥：对方的虚拟屏在线时不创建、不改镜像，直接串流当前主屏。
/// 只在主线程调用。
final class VirtualDisplayManager {
    /// 本程序虚拟屏的厂商/型号，鹰锤互联据此识别并让出显示器。
    static let vendorID: UInt32 = 0x4D53
    static let productID: UInt32 = 0x0001
    /// 鹰锤互联的“鹰锤互联 iPad Retina”虚拟屏。
    private static let yingchuiVendorID: UInt32 = 0x5943
    private static let yingchuiProductID: UInt32 = 0x4950

    private var display: CGVirtualDisplay?
    /// 虚拟屏创建时的尺寸：最大像素和物理宽高比在创建时固定。
    private var created: (width: Int, height: Int)?
    private var applied: (width: Int, height: Int, refresh: Int)?
    /// 延迟释放或延迟重建，二者只会有一个在等待。
    private var pending: DispatchWorkItem?

    /// width/height 为客户端显示器像素（虚拟屏“看起来像”这个尺寸，实际按 2 倍渲染）。
    func ensure(width: Int, height: Int, refresh: Int) {
        pending?.cancel()
        pending = nil
        guard width >= 1024, height >= 600, refresh >= 30, width <= 4096, height <= 2304 else {
            Log.info("客户端显示器参数 \(width)×\(height)@\(refresh) 不适合创建虚拟屏，跳过")
            return
        }
        // 客户端换了尺寸不同的显示器（例如 16:9 换成 1280×1024）：旧虚拟屏的最大像素和宽高比都不对，
        // 释放后稍等系统移除完毕再按新尺寸创建，否则画面仍是旧比例，被缩放进新显示器。
        if let created, created.width != width || created.height != height {
            Log.info("客户端显示器 \(created.width)×\(created.height) → \(width)×\(height)，重建虚拟屏")
            display = nil
            self.created = nil
            applied = nil
            let work = DispatchWorkItem { [self] in
                pending = nil
                ensure(width: width, height: height, refresh: refresh)
            }
            pending = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0, execute: work)
            return
        }
        if display == nil, let other = Self.onlineDisplays().first(where: Self.isYingchuiDisplay) {
            Log.info("鹰锤互联正在使用虚拟屏 \(other)，本次不创建虚拟屏，直接串流当前主屏")
            return
        }
        if let applied, applied.width == width, applied.height == height, applied.refresh == refresh,
           let display, CGMainDisplayID() == display.displayID {
            return
        }

        if display == nil {
            let descriptor = CGVirtualDisplayDescriptor()
            descriptor.queue = DispatchQueue.main
            descriptor.name = "MacStream"
            descriptor.maxPixelsWide = UInt32(width * 2)
            descriptor.maxPixelsHigh = UInt32(height * 2)
            // 按 27 英寸 16:9 的物理尺寸估算，只影响系统推荐的缩放档位。
            let diagonal = 27.0 * 25.4
            let ratio = Double(width) / Double(height)
            let heightMM = diagonal / (ratio * ratio + 1).squareRoot()
            descriptor.sizeInMillimeters = CGSize(width: heightMM * ratio, height: heightMM)
            // 固定的厂商、型号、序列号：系统据此记住这块屏的排列和模式。
            descriptor.vendorID = Self.vendorID
            descriptor.productID = Self.productID
            descriptor.serialNum = 0x0001
            descriptor.terminationHandler = { _, _ in Log.info("虚拟屏被系统移除") }
            guard let created = CGVirtualDisplay(descriptor: descriptor) else {
                Log.info("创建虚拟屏失败")
                return
            }
            display = created
            self.created = (width, height)
        }
        guard let display else { return }

        let settings = CGVirtualDisplaySettings()
        settings.hiDPI = 1
        let rates = refresh == 60 ? [60.0] : [Double(refresh), 60.0]
        settings.modes = rates.compactMap {
            CGVirtualDisplayMode(width: UInt32(width), height: UInt32(height), refreshRate: $0)
        }
        guard display.apply(settings) else {
            Log.info("虚拟屏设置模式失败")
            return
        }
        applied = (width, height, refresh)

        // 模式生效需要一点时间，稍后选定 HiDPI 模式并让其他显示器镜像虚拟屏。
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { [self] in
            configure(display.displayID, width: width, height: height, refresh: refresh)
        }
    }

    private func configure(_ id: CGDirectDisplayID, width: Int, height: Int, refresh: Int) {
        let options = [kCGDisplayShowDuplicateLowResolutionModes: true] as CFDictionary
        let modes = (CGDisplayCopyAllDisplayModes(id, options) as? [CGDisplayMode]) ?? []
        let target = modes.first {
            $0.width == width && $0.pixelWidth == width * 2 && Int($0.refreshRate.rounded()) == refresh
        }

        // 鹰锤互联的虚拟屏归它自己管理，不拉进镜像。
        let others = Self.onlineDisplays().filter { $0 != id && !Self.isYingchuiDisplay($0) }

        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success else { return }
        if let target {
            CGConfigureDisplayWithDisplayMode(config, id, target, nil)
        }
        for other in others {
            CGConfigureDisplayMirrorOfDisplay(config, other, id)
        }
        let ok = CGCompleteDisplayConfiguration(config, .forSession) == .success
        Log.info("虚拟屏 \(id)：看起来像 \(width)×\(height)，HiDPI \(width * 2)×\(height * 2) @ \(refresh) Hz，" +
                 "\(target == nil ? "未找到目标模式，" : "")其他 \(others.count) 块显示器镜像它，\(ok ? "成功" : "失败")")
    }

    /// 客户端断开后延迟释放：短时间内重连不会闪屏；释放后其他程序（鹰锤互联）才能使用显示器。
    func releaseLater(after seconds: Double = 10) {
        pending?.cancel()
        pending = nil
        guard display != nil else { return }
        let work = DispatchWorkItem { [self] in
            pending = nil
            display = nil
            created = nil
            applied = nil
            Log.info("客户端已断开 \(Int(seconds)) 秒，释放虚拟屏，镜像解除")
        }
        pending = work
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds, execute: work)
    }

    private static func isYingchuiDisplay(_ id: CGDirectDisplayID) -> Bool {
        CGDisplayVendorNumber(id) == yingchuiVendorID && CGDisplayModelNumber(id) == yingchuiProductID
    }

    private static func onlineDisplays() -> [CGDirectDisplayID] {
        var count: UInt32 = 0
        CGGetOnlineDisplayList(0, nil, &count)
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
        CGGetOnlineDisplayList(count, &ids, &count)
        return Array(ids.prefix(Int(count)))
    }
}
