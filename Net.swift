import Foundation

/// A single client TCP connection, abstracted over the platform transport:
/// Apple's `Network` framework (`NWConnection`) on macOS, raw POSIX sockets
/// (`Glibc`) on Linux. The API mirrors how `PanelServer` used `NWConnection`
/// directly — `start()` / `receive` / `send` / `cancel` / `onClose` /
/// `remoteAddress` — so the server logic is identical on both platforms.
///
/// It's a `final class` (reference type) because the server stores live
/// connections keyed by `ObjectIdentifier` for SSE broadcasts and direct
/// byte-streaming.
#if canImport(Network)
import Network

final class ClientConnection {
    private let nw: NWConnection
    /// Called once when the connection closes/fails — the server uses it to
    /// drop the connection from its SSE / direct-stream registries.
    var onClose: (() -> Void)?
    private var closeFired = false
    private let closeLock = NSLock()

    init(_ nw: NWConnection) { self.nw = nw }

    func start() {
        nw.stateUpdateHandler = { [weak self] state in
            switch state {
            case .cancelled, .failed: self?.fireClose()
            default: break
            }
        }
        nw.start(queue: .global(qos: .userInitiated))
    }

    func receive(_ completion: @escaping (Data?, _ isComplete: Bool, _ error: Error?) -> Void) {
        nw.receive(minimumIncompleteLength: 1, maximumLength: 65_536) { data, _, isComplete, error in
            completion(data, isComplete, error)
        }
    }

    func send(_ data: Data?, completion: ((Error?) -> Void)? = nil) {
        nw.send(content: data, completion: .contentProcessed { error in completion?(error) })
    }

    func cancel() { nw.cancel() }

    private func fireClose() {
        closeLock.lock(); let already = closeFired; closeFired = true; closeLock.unlock()
        if !already { onClose?() }
    }

    var remoteAddress: String {
        if case let .hostPort(host, _) = nw.endpoint {
            switch host {
            case .ipv4(let address): return "\(address)"
            case .ipv6(let address): return "\(address)"
            case .name(let name, _): return name
            @unknown default: return "unknown"
            }
        }
        return "unknown"
    }
}

#elseif os(Windows)
import WinSDK

final class ClientConnection {
    private let socket: SOCKET
    let remoteAddress: String
    var onClose: (() -> Void)?
    private let ioQueue: DispatchQueue
    private var closed = false
    private let closeLock = NSLock()

    init(socket: SOCKET, remoteAddress: String) {
        self.socket = socket
        self.remoteAddress = remoteAddress
        self.ioQueue = DispatchQueue(label: "restreamair.conn")
    }

    func start() {}

    func receive(_ completion: @escaping (Data?, _ isComplete: Bool, _ error: Error?) -> Void) {
        ioQueue.async { [socket] in
            var buffer = [UInt8](repeating: 0, count: 65_536)
            let count = buffer.withUnsafeMutableBufferPointer { ptr in
                recv(socket, UnsafeMutableRawPointer(ptr.baseAddress)?.assumingMemoryBound(to: CChar.self), Int32(ptr.count), 0)
            }
            if count > 0 {
                completion(Data(buffer[0..<Int(count)]), false, nil)
            } else if count == 0 {
                completion(nil, true, nil)
            } else {
                completion(nil, true, POSIXError(.EIO))
            }
        }
    }

    func send(_ data: Data?, completion: ((Error?) -> Void)? = nil) {
        ioQueue.async { [socket] in
            guard let data = data, !data.isEmpty else { completion?(nil); return }
            let ok = data.withUnsafeBytes { raw -> Bool in
                guard var pointer = raw.bindMemory(to: CChar.self).baseAddress else { return false }
                var remaining = Int32(data.count)
                while remaining > 0 {
                    let written = WinSDK.send(socket, pointer, remaining, 0)
                    if written <= 0 { return false }
                    pointer += Int(written)
                    remaining -= written
                }
                return true
            }
            completion?(ok ? nil : POSIXError(.EIO))
        }
    }

    func cancel() {
        closeLock.lock(); let already = closed; closed = true; closeLock.unlock()
        guard !already else { return }
        closesocket(socket)
        onClose?()
    }
}

enum POSIXServer {
    struct BindError: Error { let errnoValue: Int32 }

    static func serve(bindAddress: String, port: UInt16, onConnection: @escaping (ClientConnection) -> Void) throws {
        var wsaData = WSADATA()
        let wsaRes = WSAStartup(WORD(2 | (2 << 8)), &wsaData)
        guard wsaRes == 0 else { throw BindError(errnoValue: wsaRes) }

        let serverSocket = WinSDK.socket(AF_INET, Int32(SOCK_STREAM), Int32(IPPROTO_TCP.rawValue))
        guard serverSocket != INVALID_SOCKET else { throw BindError(errnoValue: WSAGetLastError()) }

        var reuse: Int32 = 1
        setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, UnsafeRawPointer(&reuse).assumingMemoryBound(to: CChar.self), socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_in()
        addr.sin_family = ADDRESS_FAMILY(AF_INET)
        addr.sin_port = port.bigEndian
        if bindAddress.isEmpty {
            addr.sin_addr.s_addr = in_addr_t(0)
        } else {
            _ = bindAddress.withCString { inet_pton(Int32(AF_INET), $0, &addr.sin_addr) }
        }

        let bindResult = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                WinSDK.bind(serverSocket, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bindResult == 0 else {
            let code = WSAGetLastError()
            closesocket(serverSocket)
            throw BindError(errnoValue: code)
        }
        guard listen(serverSocket, SOMAXCONN) == 0 else {
            let code = WSAGetLastError()
            closesocket(serverSocket)
            throw BindError(errnoValue: code)
        }

        let thread = Thread {
            while true {
                var clientAddr = sockaddr_in()
                var length = socklen_t(MemoryLayout<sockaddr_in>.size)
                let clientSocket = withUnsafeMutablePointer(to: &clientAddr) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        accept(serverSocket, $0, &length)
                    }
                }
                guard clientSocket != INVALID_SOCKET else {
                    Thread.sleep(forTimeInterval: 0.05)
                    continue
                }
                var timeout: DWORD = 30000
                withUnsafeBytes(of: &timeout) { ptr in
                    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, ptr.baseAddress?.assumingMemoryBound(to: CChar.self), Int32(MemoryLayout<DWORD>.size))
                }
                onConnection(ClientConnection(socket: clientSocket, remoteAddress: ipString(clientAddr)))
            }
        }
        thread.stackSize = 512 * 1024
        thread.start()
    }

    private static func ipString(_ addr: sockaddr_in) -> String {
        var source = addr.sin_addr
        let length = 16
        var buffer = [CChar](repeating: 0, count: length)
        inet_ntop(Int32(AF_INET), &source, &buffer, length)
        return String(cString: buffer)
    }
}

#else
#if canImport(Musl)
import Musl
#else
import Glibc
#endif

final class ClientConnection {
    private let fd: Int32
    let remoteAddress: String
    var onClose: (() -> Void)?
    // All reads and writes for one connection funnel through a serial queue so
    // a timer-driven SSE/direct send can't interleave with anything else on the
    // same socket.
    private let ioQueue: DispatchQueue
    private var closed = false
    private let closeLock = NSLock()

    init(fd: Int32, remoteAddress: String) {
        self.fd = fd
        self.remoteAddress = remoteAddress
        self.ioQueue = DispatchQueue(label: "restreamair.conn")
    }

    func start() {}

    func receive(_ completion: @escaping (Data?, _ isComplete: Bool, _ error: Error?) -> Void) {
        ioQueue.async { [fd] in
            var buffer = [UInt8](repeating: 0, count: 65_536)
            let count = read(fd, &buffer, buffer.count)
            if count > 0 {
                completion(Data(buffer[0..<count]), false, nil)
            } else if count == 0 {
                completion(nil, true, nil)
            } else {
                completion(nil, true, POSIXError(.EIO))
            }
        }
    }

    func send(_ data: Data?, completion: ((Error?) -> Void)? = nil) {
        ioQueue.async { [fd] in
            guard let data = data, !data.isEmpty else { completion?(nil); return }
            let ok = data.withUnsafeBytes { raw -> Bool in
                guard var pointer = raw.bindMemory(to: UInt8.self).baseAddress else { return false }
                var remaining = data.count
                while remaining > 0 {
                    let written = write(fd, pointer, remaining)
                    if written <= 0 { return false }
                    pointer += written
                    remaining -= written
                }
                return true
            }
            completion?(ok ? nil : POSIXError(.EIO))
        }
    }

    func cancel() {
        closeLock.lock(); let already = closed; closed = true; closeLock.unlock()
        guard !already else { return }
        close(fd)
        onClose?()
    }
}

/// A minimal blocking TCP accept loop for Linux, standing in for NWListener.
/// Binds `bindAddress:port` (empty address = all interfaces) and hands each
/// accepted socket to `onConnection` as a `ClientConnection`.
enum POSIXServer {
    /// Thrown so `PanelServer` can print the same friendly "port in use"
    /// message it does on macOS.
    struct BindError: Error { let errnoValue: Int32 }

    static func serve(bindAddress: String, port: UInt16, onConnection: @escaping (ClientConnection) -> Void) throws {
        // SOCK_STREAM is an enum on Glibc but a plain Int32 on Musl.
        #if canImport(Glibc)
        let streamType = Int32(SOCK_STREAM.rawValue)
        #else
        let streamType = SOCK_STREAM
        #endif
        let serverFD = socket(AF_INET, streamType, 0)
        guard serverFD >= 0 else { throw BindError(errnoValue: errno) }

        var reuse: Int32 = 1
        setsockopt(serverFD, SOL_SOCKET, SO_REUSEADDR, &reuse, socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        if bindAddress.isEmpty {
            addr.sin_addr.s_addr = in_addr_t(0) // INADDR_ANY — the C macro isn't imported by Swift
        } else {
            _ = bindAddress.withCString { inet_pton(AF_INET, $0, &addr.sin_addr) }
        }

        let bindResult = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(serverFD, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bindResult == 0 else {
            let code = errno
            close(serverFD)
            throw BindError(errnoValue: code)
        }
        guard listen(serverFD, 128) == 0 else {
            let code = errno
            close(serverFD)
            throw BindError(errnoValue: code)
        }

        let thread = Thread {
            while true {
                var clientAddr = sockaddr_in()
                var length = socklen_t(MemoryLayout<sockaddr_in>.size)
                let clientFD = withUnsafeMutablePointer(to: &clientAddr) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        accept(serverFD, $0, &length)
                    }
                }
                guard clientFD >= 0 else {
                    // Persistent accept errors (EMFILE when fds run out, etc.)
                    // would otherwise spin this loop at 100% CPU. Back off
                    // briefly; transient EINTR just retries immediately.
                    if errno != EINTR { usleep(50_000) }
                    continue
                }
                // Bound the request read so an idle client can't pin a worker
                // thread on read() forever. SSE / direct connections never read
                // again after their request, so this doesn't affect them.
                var timeout = timeval(tv_sec: 30, tv_usec: 0)
                setsockopt(clientFD, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
                onConnection(ClientConnection(fd: clientFD, remoteAddress: ipString(clientAddr)))
            }
        }
        thread.stackSize = 512 * 1024
        thread.start()
    }

    private static func ipString(_ addr: sockaddr_in) -> String {
        var source = addr.sin_addr
        let length = 16 // INET_ADDRSTRLEN — C macro, not imported by Swift
        var buffer = [CChar](repeating: 0, count: length)
        inet_ntop(AF_INET, &source, &buffer, socklen_t(length))
        return String(cString: buffer)
    }
}
#endif
