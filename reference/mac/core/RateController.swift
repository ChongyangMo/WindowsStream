import Foundation

/// 自适应码率：每 100 ms 根据发送积压估计“排队延迟”，网络拥堵时降码率、再降帧率，畅通后逐步恢复。
/// 排队延迟 = (应用层待发 + 内核未确认字节) ÷ 实测排空速度；它直接反映画面在网络上多等了多久。
final class RateController {
    struct Target: Equatable {
        var bitrate: Int  // bps
        var fps: Int
    }

    private let connection: Connection
    private let queue = DispatchQueue(label: "macstream.rate", qos: .userInitiated)
    private var timer: DispatchSourceTimer?
    private let apply: (Target) -> Void
    private let report: (_ target: Target, _ queueMs: Int, _ linkMbps: Double) -> Void

    private let maxBitrate: Int
    private let minBitrate = 6_000_000
    private let fpsLevels: [Int]
    private var fpsIndex = 0
    private var bitrate: Int

    private var lastTick = Clock.nowMicros()
    private var lastAcked: Int64 = 0
    private var linkBps: Double = 0          // 网络饱和时测得的排空速度（EWMA）
    private var goodSince = Clock.nowMicros()
    private var lastDecrease: UInt64 = 0
    private var lastIncrease: UInt64 = 0
    private var lastReport: UInt64 = 0
    private var lastApplied: Target?
    private var queueMs = 0
    private var congestedTicks = 0

    /// displayFps：显示器刷新率（帧率上限）；wireless：客户端走无线网络，起步码率上限更保守。
    init(connection: Connection, displayFps: Int, wireless: Bool,
         apply: @escaping (Target) -> Void,
         report: @escaping (_ target: Target, _ queueMs: Int, _ linkMbps: Double) -> Void) {
        self.connection = connection
        self.apply = apply
        self.report = report
        maxBitrate = wireless ? 100_000_000 : 250_000_000
        bitrate = maxBitrate
        fpsLevels = [displayFps, 60, 30].filter { $0 <= displayFps }.reduce(into: [Int]()) { levels, fps in
            if !levels.contains(fps) { levels.append(fps) }
        }
    }

    func start() {
        let stats = connection.queueStats()
        lastAcked = Int64(stats.writtenBytes) - Int64(stats.kernelQueued)
        publish()
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 0.1, repeating: 0.1)
        timer.setEventHandler { [weak self] in self?.tick() }
        timer.resume()
        self.timer = timer
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    private func tick() {
        let now = Clock.nowMicros()
        let seconds = Double(now - lastTick) / 1e6
        lastTick = now
        let stats = connection.queueStats()
        let queued = stats.appPending + stats.kernelQueued
        let acked = Int64(stats.writtenBytes) - Int64(stats.kernelQueued)
        let drained = Double(max(0, acked - lastAcked)) * 8 / max(seconds, 0.001)
        lastAcked = acked

        // 只有积压明显时，排空速度才代表链路容量（否则只是发送端没数据可发）。
        if queued > 64 * 1024 && drained > 0 {
            linkBps = linkBps == 0 ? drained : linkBps * 0.7 + drained * 0.3
        }
        let rate = max(linkBps, drained, 1_000_000)
        queueMs = Int(Double(queued) * 8 / rate * 1000)

        // 连续 3 次（0.3 秒）拥堵才降，短暂突发（连接初期的整屏无损、关键帧）不误伤。
        congestedTicks = queueMs > 40 && queued > 64 * 1024 ? congestedTicks + 1 : 0
        if congestedTicks >= 3 {
            goodSince = now
            if now - lastDecrease > 250_000 {
                lastDecrease = now
                let byLink = linkBps > 0 ? Int(linkBps * 0.8) : bitrate
                bitrate = max(minBitrate, min(Int(Double(bitrate) * 0.75), byLink))
                if queueMs > 80 && fpsIndex + 1 < fpsLevels.count {
                    fpsIndex += 1
                }
                Log.info(String(format: "网络拥堵：排队 %d ms，链路约 %.0f Mbps → 码率 %d Mbps，帧率 %d",
                                queueMs, linkBps / 1e6, bitrate / 1_000_000, fpsLevels[fpsIndex]))
            }
        } else if queueMs < 10 {
            // 持续畅通 1 秒后开始恢复：每 0.5 秒码率 +15%，畅通 2 秒以上再逐级恢复帧率。
            if now - goodSince > 1_000_000 && now - lastIncrease > 500_000 {
                lastIncrease = now
                if bitrate < maxBitrate {
                    bitrate = min(maxBitrate, Int(Double(bitrate) * 1.15))
                }
                if fpsIndex > 0 && now - goodSince > 2_000_000 {
                    fpsIndex -= 1
                }
            }
        } else {
            goodSince = now
        }

        publish()
        if now - lastReport >= 1_000_000 {
            lastReport = now
            report(currentTarget(), queueMs, linkBps / 1e6)
        }
    }

    private func currentTarget() -> Target {
        Target(bitrate: bitrate, fps: fpsLevels.isEmpty ? 60 : fpsLevels[fpsIndex])
    }

    private func publish() {
        let target = currentTarget()
        if target != lastApplied {
            lastApplied = target
            apply(target)
        }
    }
}
