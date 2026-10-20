import CryptoKit
import Darwin
import Foundation

/// 局域网自动发现：在 UDP 7301 应答客户端的广播。
/// 请求：`MSDISC1` + 16 字节随机数。
/// 应答：`MSHERE1` + u16 TCP 端口 + 32 字节 HMAC-SHA256(token, "macstream-discover" + 随机数) + 电脑名（UTF-8）。
/// HMAC 证明应答者持有同一个 token，客户端不会连到冒充的设备上。
/// 除广播外还加入组播组 239.255.77.83：两端子网掩码不一致时广播会失效，组播不受影响。
final class DiscoveryResponder {
    static let port: UInt16 = 7301
    private static let group = "239.255.77.83"
    private var joined = Set<UInt32>()
    private static let request = Array("MSDISC1".utf8)
    private static let reply = Array("MSHERE1".utf8)

    private let key: SymmetricKey
    private let tcpPort: UInt16
    private let name: String

    init(token: String, tcpPort: UInt16) {
        key = SymmetricKey(data: Array(token.utf8))
        self.tcpPort = tcpPort
        name = Host.current().localizedName ?? ProcessInfo.processInfo.hostName
    }

    func start() {
        let fd = socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else {
            Log.info("自动发现：创建 UDP 套接字失败")
            return
        }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = Self.port.bigEndian
        addr.sin_addr = in_addr(s_addr: INADDR_ANY)
        let bound = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard bound == 0 else {
            Log.info("自动发现：无法监听 UDP \(Self.port)（errno \(errno)）")
            close(fd)
            return
        }
        Log.info("自动发现：监听 UDP \(Self.port)，组播 \(Self.group)，电脑名 \(name)")
        // 网卡可能后来才连上（插网线、开热点），定期为新出现的网卡加入组播组。
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .utility))
        timer.schedule(deadline: .now(), repeating: 10)
        timer.setEventHandler { [self] in joinGroup(fd) }
        timer.resume()
        groupTimer = timer

        let thread = Thread { [self] in
            var buffer = [UInt8](repeating: 0, count: 512)
            while true {
                var from = sockaddr_in()
                var length = socklen_t(MemoryLayout<sockaddr_in>.size)
                let n = withUnsafeMutablePointer(to: &from) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, &buffer, buffer.count, 0, $0, &length) }
                }
                guard n == Self.request.count + 16, Array(buffer[0..<Self.request.count]) == Self.request else { continue }
                let nonce = Array(buffer[Self.request.count..<n])
                let proof = HMAC<SHA256>.authenticationCode(for: Array("macstream-discover".utf8) + nonce, using: key)
                var message = Self.reply
                message.append(UInt8(tcpPort & 0xFF))
                message.append(UInt8(tcpPort >> 8))
                message.append(contentsOf: Array(proof))
                message.append(contentsOf: Array(name.utf8.prefix(64)))
                _ = withUnsafePointer(to: &from) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sendto(fd, message, message.count, 0, $0, length) }
                }
            }
        }
        thread.name = "macstream.discovery"
        thread.start()
    }

    private var groupTimer: DispatchSourceTimer?

    private func joinGroup(_ fd: Int32) {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return }
        defer { freeifaddrs(list) }
        var current = Set<UInt32>()
        for entry in sequence(first: first, next: { $0.pointee.ifa_next }) {
            guard let address = entry.pointee.ifa_addr, address.pointee.sa_family == sa_family_t(AF_INET),
                  entry.pointee.ifa_flags & UInt32(IFF_LOOPBACK) == 0, entry.pointee.ifa_flags & UInt32(IFF_UP) != 0 else { continue }
            let local = address.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee.sin_addr }
            current.insert(local.s_addr)
            guard !joined.contains(local.s_addr) else { continue }
            var request = ip_mreq(imr_multiaddr: in_addr(s_addr: inet_addr(Self.group)), imr_interface: local)
            if setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, socklen_t(MemoryLayout<ip_mreq>.size)) == 0 || errno == EADDRINUSE {
                joined.insert(local.s_addr)
            }
        }
        joined.formIntersection(current)  // 网卡地址消失后允许以后重新加入
    }
}
