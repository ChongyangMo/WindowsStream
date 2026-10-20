import AppKit
import ApplicationServices
import CoreGraphics
import ServiceManagement

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
Log.info("MacStream 启动，pid \(getpid())")

let config = Config.loadOrCreate()

// 登录项：按配置注册或取消，状态以系统为准（用户也可以在系统设置里关掉）。
let loginItem = SMAppService.mainApp
if config.launchAtLogin && loginItem.status != .enabled {
    do {
        try loginItem.register()
        Log.info("已注册为登录项（系统设置 → 通用 → 登录项 可关闭）")
    } catch {
        Log.info("注册登录项失败：\(error.localizedDescription)")
    }
} else if !config.launchAtLogin && loginItem.status == .enabled {
    try? loginItem.unregister()
    Log.info("已取消登录项")
}
Log.info("登录项状态：\(loginItem.status == .enabled ? "已启用" : loginItem.status == .requiresApproval ? "等待在系统设置中批准" : "未启用")")

if !CGPreflightScreenCaptureAccess() {
    Log.info("尚未获得屏幕录制权限，已请求授权：系统设置 → 隐私与安全性 → 录屏与系统录音 → 允许 MacStream")
    CGRequestScreenCaptureAccess()
}
let axOptions = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
if !AXIsProcessTrustedWithOptions(axOptions) {
    Log.info("尚未获得辅助功能权限（键鼠控制需要）：系统设置 → 隐私与安全性 → 辅助功能 → 允许 MacStream")
}

let session = Session(config: config)
let server = Server(port: config.port) { session.accept($0) }
do {
    try server.start()
} catch {
    Log.info("无法监听端口 \(config.port)：\(error)")
    exit(1)
}
let discovery = DiscoveryResponder(token: config.token, tcpPort: config.port)
discovery.start()

app.run()
