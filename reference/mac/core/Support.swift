import Foundation
import Security

enum Log {
    private static let lock = NSLock()
    private static let url: URL = {
        let dir = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("MacStream.log")
    }()
    private static let formatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"
        return f
    }()

    static func info(_ message: String) {
        lock.lock()
        defer { lock.unlock() }
        let line = "\(formatter.string(from: Date())) \(message)\n"
        FileHandle.standardError.write(line.data(using: .utf8)!)
        if let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
           let size = attrs[.size] as? Int, size > 4 * 1024 * 1024 {
            try? FileManager.default.removeItem(at: url)
        }
        if let handle = try? FileHandle(forWritingTo: url) {
            handle.seekToEndOfFile()
            handle.write(line.data(using: .utf8)!)
            try? handle.close()
        } else {
            try? line.data(using: .utf8)!.write(to: url)
        }
    }
}

/// 单调时钟（微秒），用于延迟测量。
enum Clock {
    static func nowMicros() -> UInt64 { clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000 }

    private static let timebase: mach_timebase_info_data_t = {
        var info = mach_timebase_info_data_t()
        mach_timebase_info(&info)
        return info
    }()

    /// mach_absolute_time 刻度 → 微秒（与 nowMicros 同一时间轴）。
    static func micros(machTicks: UInt64) -> UInt64 {
        machTicks / 1000 * UInt64(timebase.numer) / UInt64(timebase.denom)
    }
}

struct Config: Codable {
    var port: UInt16 = 7300
    var token: String = ""
    /// 最高帧率；实际取它和显示器刷新率中较小的一个。
    var fps: Int = 120
    /// 0 = 按分辨率和帧率自动（1080p60 约 80 Mbps，上限 150）。
    var bitrateMbps: Int = 0
    /// 画面静止后把变化区域无损发送（像素级清晰）；关闭时改用视频“补清晰”帧。
    var losslessStatic: Bool = true
    /// 视频帧允许的最大量化参数，越小运动画面越清晰、码率越高（H.264 范围 0–51）。
    var maxQP: Int = 28
    /// 按客户端显示器创建匹配分辨率和刷新率的 HiDPI 虚拟屏（例如 2560×1440 @ 100 Hz）。
    var virtualDisplay: Bool = true
    /// 登录 Mac 后自动启动（通过 SMAppService 注册为登录项，可在“系统设置 → 通用 → 登录项”中关闭）。
    var launchAtLogin: Bool = false

    init() {}

    /// 缺少的字段使用默认值，旧配置文件可以直接沿用。
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = Config()
        port = try c.decodeIfPresent(UInt16.self, forKey: .port) ?? d.port
        token = try c.decodeIfPresent(String.self, forKey: .token) ?? d.token
        fps = try c.decodeIfPresent(Int.self, forKey: .fps) ?? d.fps
        bitrateMbps = try c.decodeIfPresent(Int.self, forKey: .bitrateMbps) ?? d.bitrateMbps
        losslessStatic = try c.decodeIfPresent(Bool.self, forKey: .losslessStatic) ?? d.losslessStatic
        maxQP = try c.decodeIfPresent(Int.self, forKey: .maxQP) ?? d.maxQP
        virtualDisplay = try c.decodeIfPresent(Bool.self, forKey: .virtualDisplay) ?? d.virtualDisplay
        launchAtLogin = try c.decodeIfPresent(Bool.self, forKey: .launchAtLogin) ?? d.launchAtLogin
    }

    static let url = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/MacStream/config.json")

    /// 读取配置；首次运行时生成随机 token 并以 600 权限保存。
    static func loadOrCreate() -> Config {
        if let data = try? Data(contentsOf: url), let config = try? JSONDecoder().decode(Config.self, from: data),
           !config.token.isEmpty {
            return config
        }
        var config = Config()
        var bytes = [UInt8](repeating: 0, count: 32)
        _ = SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes)
        config.token = bytes.map { String(format: "%02x", $0) }.joined()
        do {
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            try encoder.encode(config).write(to: url)
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
            Log.info("已生成新配置 \(url.path)")
        } catch {
            Log.info("保存配置失败：\(error)")
        }
        return config
    }
}

struct ByteWriter {
    private(set) var bytes: [UInt8] = []

    init(capacity: Int = 64) { bytes.reserveCapacity(capacity) }

    mutating func u8(_ v: UInt8) { bytes.append(v) }
    mutating func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { bytes.append(contentsOf: $0) } }
    mutating func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { bytes.append(contentsOf: $0) } }
    mutating func u64(_ v: UInt64) { withUnsafeBytes(of: v.littleEndian) { bytes.append(contentsOf: $0) } }
    mutating func append(_ b: [UInt8]) { bytes.append(contentsOf: b) }
}

struct ByteReader {
    let bytes: [UInt8]
    private(set) var offset = 0

    init(_ bytes: [UInt8]) { self.bytes = bytes }

    var remaining: Int { bytes.count - offset }

    mutating func u8() throws -> UInt8 {
        guard remaining >= 1 else { throw ProtocolError.truncated }
        defer { offset += 1 }
        return bytes[offset]
    }

    mutating func u16() throws -> UInt16 {
        guard remaining >= 2 else { throw ProtocolError.truncated }
        defer { offset += 2 }
        return UInt16(bytes[offset]) | UInt16(bytes[offset + 1]) << 8
    }

    mutating func i16() throws -> Int16 { Int16(bitPattern: try u16()) }

    mutating func u32() throws -> UInt32 {
        UInt32(try u16()) | UInt32(try u16()) << 16
    }

    mutating func u64() throws -> UInt64 {
        UInt64(try u32()) | UInt64(try u32()) << 32
    }

    mutating func take(_ count: Int) throws -> [UInt8] {
        guard remaining >= count else { throw ProtocolError.truncated }
        defer { offset += count }
        return Array(bytes[offset..<offset + count])
    }

    mutating func rest() -> [UInt8] {
        defer { offset = bytes.count }
        return Array(bytes[offset...])
    }
}

enum ProtocolError: Error {
    case truncated
    case tooLarge(Int)
    case closed
    case unexpected(UInt8)
    case authFailed
}

enum MessageType: UInt8 {
    case serverHello = 1
    case clientAuth = 2
    case authResult = 3
    case video = 10
    case cursor = 11
    case lossless = 12
    case ping = 20
    case pong = 21
    case stats = 22
    case clipboard = 30
    case mouseMove = 40
    case mouseButton = 41
    case scroll = 42
    case key = 43
    case releaseAll = 44
    case viewSize = 45
    case monitorMode = 46
    case requestKeyframe = 50
}

let protocolVersion: UInt32 = 1
