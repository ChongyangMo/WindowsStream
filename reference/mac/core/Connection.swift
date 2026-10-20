import Darwin
import Foundation

/// 一个客户端 TCP 连接：阻塞读取在调用线程，发送走串行队列；视频帧积压时可丢弃。
final class Connection {
    let peer: String
    private let fd: Int32
    private let sendQueue = DispatchQueue(label: "macstream.send", qos: .userInteractive)
    private let lock = NSLock()
    private var pendingVideoFrames = 0
    private var pendingBytes = 0
    private var writtenBytes: UInt64 = 0
    private var closed = false

    /// 积压超过这个数量的视频帧时丢弃新帧（宁可丢帧也不让延迟累积）。
    private let maxPendingVideoFrames = 2
    /// 积压字节超过这个值时丢弃视频帧（含关键帧），避免关键帧越堆越多。
    private let maxQueuedBytes = 1536 * 1024

    /// 采集端限流用：网络积压足够少、可以编码下一帧。
    func canAcceptVideo(belowBytes bytes: Int) -> Bool {
        let stats = queueStats()
        lock.lock()
        let frames = pendingVideoFrames
        lock.unlock()
        return frames < maxPendingVideoFrames && stats.appPending + stats.kernelQueued < bytes
    }

    init(fd: Int32, peer: String) {
        self.fd = fd
        self.peer = peer
        var one: Int32 = 1
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        // 内核发送缓冲不宜过大：数据积压在内核里会变成看不见的延迟（低速 Wi-Fi 下 1 MB 就是几百毫秒）。
        // 256 KB 足够千兆有线和 Wi-Fi 的带宽时延积。
        var sendBuffer: Int32 = 256 * 1024
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendBuffer, socklen_t(MemoryLayout<Int32>.size))
    }

    /// 发送积压：应用层队列里待写的字节、内核发送缓冲里尚未被对方确认的字节，以及累计写入内核的字节数。
    func queueStats() -> (appPending: Int, kernelQueued: Int, writtenBytes: UInt64) {
        var kernel: Int32 = 0
        var length = socklen_t(MemoryLayout<Int32>.size)
        if getsockopt(fd, SOL_SOCKET, SO_NWRITE, &kernel, &length) != 0 {
            kernel = 0
        }
        lock.lock()
        defer { lock.unlock() }
        return (pendingBytes, Int(kernel), writtenBytes)
    }

    /// 积压是否足够少，可以插入一批无损数据而不耽误视频。
    func isDrained(below bytes: Int) -> Bool {
        let stats = queueStats()
        return stats.appPending + stats.kernelQueued < bytes
    }

    var isClosed: Bool {
        lock.lock()
        defer { lock.unlock() }
        return closed
    }

    /// 发送消息。video 为 true 且不是关键帧时，积压过多会返回 false（已丢弃）。
    /// completion 在这条消息写入套接字之后（或连接失败时）在发送队列上调用。
    @discardableResult
    func send(_ type: MessageType, _ payload: [UInt8], droppableVideo: Bool = false, isVideo: Bool = false,
              completion: (() -> Void)? = nil) -> Bool {
        // 最后一道保险：积压过多时连关键帧也丢（正常情况下采集端会先限流，不会走到这里）。
        if droppableVideo {
            let stats = queueStats()
            if stats.appPending + stats.kernelQueued > maxQueuedBytes {
                return false
            }
        }
        lock.lock()
        if closed || (droppableVideo && pendingVideoFrames >= maxPendingVideoFrames) {
            lock.unlock()
            return false
        }
        if isVideo {
            pendingVideoFrames += 1
        }
        pendingBytes += payload.count + 5
        lock.unlock()

        var frame = ByteWriter(capacity: payload.count + 5)
        frame.u32(UInt32(payload.count))
        frame.u8(type.rawValue)
        frame.append(payload)
        let bytes = frame.bytes
        sendQueue.async { [self] in
            var ok = true
            do {
                try writeAll(bytes)
            } catch {
                ok = false
                close()
            }
            lock.lock()
            if isVideo {
                pendingVideoFrames -= 1
            }
            pendingBytes -= bytes.count
            if ok {
                writtenBytes += UInt64(bytes.count)
            }
            lock.unlock()
            completion?()
        }
        return true
    }

    /// 0 表示不超时。
    func setReadTimeout(seconds: Int) {
        var timeout = timeval(tv_sec: seconds, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
    }

    func readMessage() throws -> (UInt8, [UInt8]) {
        let header = try readExact(5)
        let length = Int(UInt32(header[0]) | UInt32(header[1]) << 8 | UInt32(header[2]) << 16 | UInt32(header[3]) << 24)
        guard length <= 16 * 1024 * 1024 else { throw ProtocolError.tooLarge(length) }
        return (header[4], try readExact(length))
    }

    func close() {
        lock.lock()
        let wasClosed = closed
        closed = true
        lock.unlock()
        if !wasClosed {
            shutdown(fd, SHUT_RDWR)
        }
    }

    /// 读线程退出后调用，真正释放文件描述符。
    func release() {
        close()
        sendQueue.async { [fd] in Darwin.close(fd) }
    }

    private func readExact(_ count: Int) throws -> [UInt8] {
        var buffer = [UInt8](repeating: 0, count: count)
        var offset = 0
        while offset < count {
            let n = buffer.withUnsafeMutableBytes { recv(fd, $0.baseAddress! + offset, count - offset, 0) }
            if n <= 0 {
                throw ProtocolError.closed
            }
            offset += n
        }
        return buffer
    }

    private func writeAll(_ bytes: [UInt8]) throws {
        try bytes.withUnsafeBytes { raw in
            var offset = 0
            while offset < raw.count {
                let n = Darwin.send(fd, raw.baseAddress! + offset, raw.count - offset, 0)
                if n <= 0 {
                    if n < 0 && errno == EINTR { continue }
                    throw ProtocolError.closed
                }
                offset += n
            }
        }
    }
}

final class Server {
    private let port: UInt16
    private let onAccept: (Connection) -> Void

    init(port: UInt16, onAccept: @escaping (Connection) -> Void) {
        self.port = port
        self.onAccept = onAccept
    }

    func start() throws {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw POSIXError(.init(rawValue: errno) ?? .EIO) }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr = in_addr(s_addr: INADDR_ANY)
        let bound = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard bound == 0, listen(fd, 4) == 0 else {
            let code = errno
            Darwin.close(fd)
            throw POSIXError(.init(rawValue: code) ?? .EIO)
        }
        Log.info("监听 TCP 端口 \(port)")

        let thread = Thread { [onAccept] in
            while true {
                var client = sockaddr_in()
                var length = socklen_t(MemoryLayout<sockaddr_in>.size)
                let cfd = withUnsafeMutablePointer(to: &client) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { accept(fd, $0, &length) }
                }
                if cfd < 0 {
                    if errno == EINTR { continue }
                    Log.info("accept 失败：errno \(errno)")
                    Thread.sleep(forTimeInterval: 0.5)
                    continue
                }
                var text = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                inet_ntop(AF_INET, &client.sin_addr, &text, socklen_t(INET_ADDRSTRLEN))
                onAccept(Connection(fd: cfd, peer: String(cString: text)))
            }
        }
        thread.name = "macstream.accept"
        thread.start()
    }
}
