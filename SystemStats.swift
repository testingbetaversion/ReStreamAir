import Foundation
#if os(macOS)
import Darwin
#elseif os(Linux)
import Glibc
#endif

final class SystemStats {
    private let lock = NSLock()

    #if os(macOS)
    private var previousCPUTicks: (user: UInt32, system: UInt32, idle: UInt32, nice: UInt32)?
    #elseif os(Linux)
    private var previousLinuxTicks: (user: UInt64, system: UInt64, idle: UInt64, nice: UInt64, total: UInt64)?
    #endif

    lazy var cpuBrand: String = {
        #if os(macOS)
        var size = 0
        sysctlbyname("machdep.cpu.brand_string", nil, &size, nil, 0)
        guard size > 0 else { return "Unknown CPU" }
        var buffer = [CChar](repeating: 0, count: size)
        sysctlbyname("machdep.cpu.brand_string", &buffer, &size, nil, 0)
        return String(cString: buffer)
        #else
        return "Unknown CPU"
        #endif
    }()

    var coreCount: Int { ProcessInfo.processInfo.processorCount }
    var totalMemoryBytes: UInt64 { ProcessInfo.processInfo.physicalMemory }
    var osVersion: String { ProcessInfo.processInfo.operatingSystemVersionString }
    var uptimeSeconds: Double { ProcessInfo.processInfo.systemUptime }

    /// 1-minute load average (POSIX getloadavg, available on both macOS and
    /// Linux) — shown alongside core count as "x.xx / N cores" rather than a
    /// single CPU% figure, matching what most server monitoring tools show.
    func loadAverage() -> Double {
        var loads = [Double](repeating: 0, count: 3)
        let count = getloadavg(&loads, 3)
        guard count > 0 else { return 0 }
        return loads[0]
    }

    func diskStats() -> (total: Int64, available: Int64) {
        guard let values = try? URL(fileURLWithPath: "/").resourceValues(forKeys: [.volumeTotalCapacityKey, .volumeAvailableCapacityKey]) else {
            return (0, 0)
        }
        return (Int64(values.volumeTotalCapacity ?? 0), Int64(values.volumeAvailableCapacity ?? 0))
    }

    func cpuPercent() -> Double {
        #if os(macOS)
        var cpuLoad = host_cpu_load_info()
        var count = mach_msg_type_number_t(MemoryLayout<host_cpu_load_info_data_t>.size / MemoryLayout<integer_t>.size)
        let result = withUnsafeMutablePointer(to: &cpuLoad) { pointer -> kern_return_t in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { intPointer in
                host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, intPointer, &count)
            }
        }
        guard result == KERN_SUCCESS else { return 0 }
        let user = cpuLoad.cpu_ticks.0
        let system = cpuLoad.cpu_ticks.1
        let idle = cpuLoad.cpu_ticks.2
        let nice = cpuLoad.cpu_ticks.3

        lock.lock()
        let previous = previousCPUTicks
        previousCPUTicks = (user, system, idle, nice)
        lock.unlock()

        guard let previous else { return 0 }
        let userDelta = Double(user &- previous.user)
        let systemDelta = Double(system &- previous.system)
        let idleDelta = Double(idle &- previous.idle)
        let niceDelta = Double(nice &- previous.nice)
        let totalDelta = userDelta + systemDelta + idleDelta + niceDelta
        guard totalDelta > 0 else { return 0 }
        return max(0, min(100, ((userDelta + systemDelta + niceDelta) / totalDelta) * 100))
        #elseif os(Linux)
        return linuxCPUPercent()
        #else
        return 0
        #endif
    }

    func memoryUsedBytes() -> UInt64 {
        #if os(macOS)
        var stats = vm_statistics64()
        var count = mach_msg_type_number_t(MemoryLayout<vm_statistics64_data_t>.size / MemoryLayout<integer_t>.size)
        let result = withUnsafeMutablePointer(to: &stats) { pointer -> kern_return_t in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { intPointer in
                host_statistics64(mach_host_self(), HOST_VM_INFO64, intPointer, &count)
            }
        }
        guard result == KERN_SUCCESS else { return 0 }
        let pageSize = UInt64(vm_kernel_page_size)
        return UInt64(stats.active_count + stats.inactive_count + stats.wire_count + stats.speculative_count) * pageSize
        #elseif os(Linux)
        return linuxMemoryUsedBytes()
        #else
        return 0
        #endif
    }

    #if os(Linux)
    private func linuxCPUPercent() -> Double {
        guard let content = try? String(contentsOfFile: "/proc/stat", encoding: .utf8),
              let line = content.split(separator: "\n").first(where: { $0.hasPrefix("cpu ") }) else { return 0 }
        let fields = line.split(separator: " ").dropFirst().compactMap { UInt64($0) }
        guard fields.count >= 4 else { return 0 }
        let user = fields[0], nice = fields[1], system = fields[2], idle = fields[3]
        let total = user + nice + system + idle

        lock.lock()
        let previous = previousLinuxTicks
        previousLinuxTicks = (user, system, idle, nice, total)
        lock.unlock()

        guard let previous else { return 0 }
        let totalDelta = Double(total &- previous.total)
        guard totalDelta > 0 else { return 0 }
        let idleDelta = Double(idle &- previous.idle)
        return max(0, min(100, (1 - idleDelta / totalDelta) * 100))
    }

    private func linuxMemoryUsedBytes() -> UInt64 {
        guard let content = try? String(contentsOfFile: "/proc/meminfo", encoding: .utf8) else { return 0 }
        var total: UInt64 = 0
        var available: UInt64 = 0
        for line in content.split(separator: "\n") {
            let fields = line.split(separator: " ").compactMap { UInt64($0) }
            if line.hasPrefix("MemTotal:") { total = (fields.first ?? 0) * 1024 }
            if line.hasPrefix("MemAvailable:") { available = (fields.first ?? 0) * 1024 }
        }
        return total > available ? total - available : 0
    }
    #endif

    func snapshotView(peaks: SystemPeaks) -> [String: Any] {
        let cpu = cpuPercent()
        let memUsed = memoryUsedBytes()
        let disk = diskStats()
        return [
            "cpuModel": cpuBrand,
            "coreCount": coreCount,
            "totalMemoryBytes": totalMemoryBytes,
            "osVersion": osVersion,
            "uptimeSeconds": uptimeSeconds,
            "diskTotalBytes": disk.total,
            "diskAvailableBytes": disk.available,
            "cpuPercent": cpu,
            "loadAverage": loadAverage(),
            "memoryUsedBytes": memUsed,
            "peakCPUPercent": max(peaks.maxCPUPercent, cpu),
            "peakMemoryBytes": max(peaks.maxMemoryBytes, memUsed)
        ]
    }
}
