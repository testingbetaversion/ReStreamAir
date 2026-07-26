#include "rs_sysstats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct rs_sysstats {
    // Previous CPU tick sample, for computing the percentage between polls.
    unsigned long long prev_total;
    unsigned long long prev_idle;
    bool have_prev;
    double peak_cpu;
    unsigned long long peak_mem;
};

rs_sysstats *rs_sysstats_create(void) {
    return (rs_sysstats *)calloc(1, sizeof(rs_sysstats));
}

void rs_sysstats_destroy(rs_sysstats *stats) {
    free(stats);
}

// Per-platform primitives fill this flat struct; the JSON assembly at the
// bottom is shared.
typedef struct {
    double cpu_percent;      // computed from the tick delta
    char cpu_model[256];
    int core_count;
    double load_average;
    unsigned long long mem_used;
    unsigned long long mem_total;
    unsigned long long disk_total;
    unsigned long long disk_available;
    long long uptime_seconds;
    char os_version[128];
} raw_stats;

// Reads cumulative CPU ticks (total and idle). Returns false if unavailable.
static bool read_cpu_ticks(unsigned long long *total, unsigned long long *idle);
static void read_platform(raw_stats *r);

#if defined(__APPLE__)

#include <mach/mach.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <unistd.h>

static bool read_cpu_ticks(unsigned long long *total, unsigned long long *idle) {
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&info, &count) != KERN_SUCCESS) {
        return false;
    }
    unsigned long long sum = 0;
    for (int i = 0; i < CPU_STATE_MAX; i++) sum += info.cpu_ticks[i];
    *total = sum;
    *idle = info.cpu_ticks[CPU_STATE_IDLE];
    return true;
}

static void read_platform(raw_stats *r) {
    size_t len;

    len = sizeof(r->cpu_model);
    if (sysctlbyname("machdep.cpu.brand_string", r->cpu_model, &len, NULL, 0) != 0) {
        len = sizeof(r->cpu_model);
        if (sysctlbyname("hw.model", r->cpu_model, &len, NULL, 0) != 0) {
            strncpy(r->cpu_model, "Apple", sizeof(r->cpu_model) - 1);
        }
    }

    int ncpu = 0;
    len = sizeof(ncpu);
    if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0) r->core_count = ncpu;

    unsigned long long memsize = 0;
    len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0) r->mem_total = memsize;

    // Used memory = (active + wired + compressed) pages.
    vm_statistics64_data_t vm;
    mach_msg_type_number_t vm_count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &vm_count) == KERN_SUCCESS) {
        unsigned long long page = (unsigned long long)sysconf(_SC_PAGESIZE);
        r->mem_used = ((unsigned long long)vm.active_count + vm.wire_count + vm.compressor_page_count) * page;
    }

    double loads[3];
    if (getloadavg(loads, 3) > 0) r->load_average = loads[0];

    struct statfs fs;
    if (statfs("/", &fs) == 0) {
        r->disk_total = (unsigned long long)fs.f_blocks * fs.f_bsize;
        r->disk_available = (unsigned long long)fs.f_bavail * fs.f_bsize;
    }

    struct timeval boot;
    len = sizeof(boot);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boot, &len, NULL, 0) == 0 && boot.tv_sec != 0) {
        r->uptime_seconds = (long long)time(NULL) - (long long)boot.tv_sec;
    }

    char release[64] = {0};
    len = sizeof(release);
    if (sysctlbyname("kern.osrelease", release, &len, NULL, 0) == 0) {
        snprintf(r->os_version, sizeof(r->os_version), "macOS (Darwin %s)", release);
    } else {
        strncpy(r->os_version, "macOS", sizeof(r->os_version) - 1);
    }
}

#elif defined(__linux__)

#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

static bool read_cpu_ticks(unsigned long long *total, unsigned long long *idle) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return false;
    unsigned long long user, nice, sys, idle_t, iowait, irq, softirq, steal;
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &sys, &idle_t, &iowait, &irq, &softirq, &steal);
    fclose(f);
    if (n < 4) return false;
    *idle = idle_t + (n >= 5 ? iowait : 0);
    *total = user + nice + sys + idle_t + (n >= 5 ? iowait : 0) + (n >= 6 ? irq : 0) +
             (n >= 7 ? softirq : 0) + (n >= 8 ? steal : 0);
    return true;
}

static void read_platform(raw_stats *r) {
    r->core_count = (int)sysconf(_SC_NPROCESSORS_ONLN);

    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo) {
        char line[256];
        while (fgets(line, sizeof(line), cpuinfo)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) {
                    char *value = colon + 1;
                    while (*value == ' ') value++;
                    size_t vlen = strlen(value);
                    while (vlen > 0 && (value[vlen - 1] == '\n' || value[vlen - 1] == ' ')) vlen--;
                    snprintf(r->cpu_model, sizeof(r->cpu_model), "%.*s", (int)vlen, value);
                }
                break;
            }
        }
        fclose(cpuinfo);
    }
    if (!r->cpu_model[0]) strncpy(r->cpu_model, "CPU", sizeof(r->cpu_model) - 1);

    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        r->mem_total = (unsigned long long)info.totalram * info.mem_unit;
        r->uptime_seconds = info.uptime;
        r->load_average = (double)info.loads[0] / 65536.0;
    }
    // MemAvailable is a better "free" figure than sysinfo's freeram.
    FILE *meminfo = fopen("/proc/meminfo", "r");
    if (meminfo) {
        char line[128];
        unsigned long long total_kb = 0, avail_kb = 0;
        while (fgets(line, sizeof(line), meminfo)) {
            if (sscanf(line, "MemTotal: %llu kB", &total_kb) == 1) r->mem_total = total_kb * 1024;
            else if (sscanf(line, "MemAvailable: %llu kB", &avail_kb) == 1) { /* below */ }
        }
        if (total_kb && avail_kb) r->mem_used = (total_kb - avail_kb) * 1024;
        fclose(meminfo);
    }

    struct statvfs fs;
    if (statvfs("/", &fs) == 0) {
        r->disk_total = (unsigned long long)fs.f_blocks * fs.f_frsize;
        r->disk_available = (unsigned long long)fs.f_bavail * fs.f_frsize;
    }

    struct utsname uts;
    if (uname(&uts) == 0) {
        snprintf(r->os_version, sizeof(r->os_version), "%s %s", uts.sysname, uts.release);
    } else {
        strncpy(r->os_version, "Linux", sizeof(r->os_version) - 1);
    }
}

#else  // Windows and any other platform: a minimal, never-crashing fallback.

#if defined(_WIN32)
#include <windows.h>
#endif

static bool read_cpu_ticks(unsigned long long *total, unsigned long long *idle) {
#if defined(_WIN32)
    FILETIME idle_ft, kernel_ft, user_ft;
    if (!GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) return false;
    ULARGE_INTEGER i, k, u;
    i.LowPart = idle_ft.dwLowDateTime;   i.HighPart = idle_ft.dwHighDateTime;
    k.LowPart = kernel_ft.dwLowDateTime; k.HighPart = kernel_ft.dwHighDateTime;
    u.LowPart = user_ft.dwLowDateTime;   u.HighPart = user_ft.dwHighDateTime;
    *idle = i.QuadPart;
    *total = k.QuadPart + u.QuadPart;  // kernel time already includes idle
    return true;
#else
    (void)total; (void)idle;
    return false;
#endif
}

static void read_platform(raw_stats *r) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    r->core_count = (int)si.dwNumberOfProcessors;
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        r->mem_total = mem.ullTotalPhys;
        r->mem_used = mem.ullTotalPhys - mem.ullAvailPhys;
    }
    ULARGE_INTEGER free_bytes, total_bytes;
    if (GetDiskFreeSpaceExA("C:\\", &free_bytes, &total_bytes, NULL)) {
        r->disk_total = total_bytes.QuadPart;
        r->disk_available = free_bytes.QuadPart;
    }
    r->uptime_seconds = (long long)(GetTickCount64() / 1000);
    strncpy(r->cpu_model, "CPU", sizeof(r->cpu_model) - 1);
    strncpy(r->os_version, "Windows", sizeof(r->os_version) - 1);
#else
    strncpy(r->cpu_model, "CPU", sizeof(r->cpu_model) - 1);
    strncpy(r->os_version, "Unknown", sizeof(r->os_version) - 1);
#endif
}

#endif

rs_json *rs_sysstats_snapshot(rs_sysstats *stats) {
    raw_stats r;
    memset(&r, 0, sizeof(r));
    read_platform(&r);

    // CPU percent from the delta between this tick sample and the last.
    unsigned long long total = 0, idle = 0;
    if (read_cpu_ticks(&total, &idle) && stats->have_prev && total > stats->prev_total) {
        unsigned long long dt = total - stats->prev_total;
        unsigned long long di = idle - stats->prev_idle;
        r.cpu_percent = dt > 0 ? (1.0 - (double)di / (double)dt) * 100.0 : 0.0;
    }
    if (total > 0) {
        stats->prev_total = total;
        stats->prev_idle = idle;
        stats->have_prev = true;
    }
    if (r.cpu_percent < 0) r.cpu_percent = 0;
    if (r.cpu_percent > 100) r.cpu_percent = 100;

    // Running peaks (in-memory; reset on restart).
    if (r.cpu_percent > stats->peak_cpu) stats->peak_cpu = r.cpu_percent;
    if (r.mem_used > stats->peak_mem) stats->peak_mem = r.mem_used;

    rs_json *out = rs_json_new_obj();
    rs_json_obj_set(out, "cpuPercent", rs_json_new_num(r.cpu_percent));
    rs_json_obj_set_str(out, "cpuModel", r.cpu_model);
    rs_json_obj_set_int(out, "coreCount", r.core_count);
    rs_json_obj_set(out, "loadAverage", rs_json_new_num(r.load_average));
    rs_json_obj_set_int(out, "memoryUsedBytes", (long long)r.mem_used);
    rs_json_obj_set_int(out, "totalMemoryBytes", (long long)r.mem_total);
    rs_json_obj_set_int(out, "diskTotalBytes", (long long)r.disk_total);
    rs_json_obj_set_int(out, "diskAvailableBytes", (long long)r.disk_available);
    rs_json_obj_set_int(out, "uptimeSeconds", r.uptime_seconds);
    rs_json_obj_set_str(out, "osVersion", r.os_version);
    rs_json_obj_set(out, "peakCPUPercent", rs_json_new_num(stats->peak_cpu));
    rs_json_obj_set_int(out, "peakMemoryBytes", (long long)stats->peak_mem);
    return out;
}
