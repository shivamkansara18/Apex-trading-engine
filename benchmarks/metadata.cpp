#include "apex/metrics/benchmark.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace apex {
MachineMetadata machine_metadata() {
    MachineMetadata result;
    struct utsname info {};
    if (::uname(&info) == 0) { result.os = info.sysname; result.kernel = info.release; result.architecture = info.machine; }
    result.logical_cpus = std::thread::hardware_concurrency();
    result.cpu = "unavailable"; result.governor = "unavailable"; result.affinity = "uncontrolled";
#ifdef __APPLE__
    char cpu[256]{};
    std::size_t size = sizeof(cpu);
    if (::sysctlbyname("machdep.cpu.brand_string", cpu, &size, nullptr, 0) == 0) { result.cpu = cpu; }
    size = sizeof(result.ram_bytes);
    if (::sysctlbyname("hw.memsize", &result.ram_bytes, &size, nullptr, 0) != 0) { result.ram_bytes = 0; }
    result.affinity = "uncontrolled; macOS has no Linux taskset equivalent";
#elif defined(__linux__)
    std::ifstream cpu("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpu, line)) {
        if (line.starts_with("model name") && line.find(':') != std::string::npos) {
            result.cpu = line.substr(line.find(':') + 2); break;
        }
    }
    const auto pages = ::sysconf(_SC_PHYS_PAGES);
    const auto page_size = ::sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) { result.ram_bytes = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size); }
    std::ifstream governor("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    std::string value;
    if (governor >> value) { result.governor = value; }
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (::sched_getaffinity(0, sizeof(affinity), &affinity) == 0) {
        result.affinity.clear();
        for (int index = 0; index < CPU_SETSIZE; ++index) {
            if (CPU_ISSET(index, &affinity)) { result.affinity += std::to_string(index) + ","; }
        }
    }
#endif
    struct rusage usage {};
    if (::getrusage(RUSAGE_SELF, &usage) == 0) {
        result.process_peak_rss_bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#ifdef __linux__
        result.process_peak_rss_bytes *= 1024;
#endif
    }
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    ::gmtime_r(&now, &utc);
    std::ostringstream timestamp;
    timestamp << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    result.timestamp_utc = timestamp.str();
    std::vector<std::uint64_t> pairs;
    pairs.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        const auto before = std::chrono::steady_clock::now();
        const auto after = std::chrono::steady_clock::now();
        const auto duration = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
        pairs.push_back(duration);
        if (duration == 0) { ++result.clock_pair_zero_count; }
        else if (result.clock_pair_min_positive_ns == 0 || duration < result.clock_pair_min_positive_ns) {
            result.clock_pair_min_positive_ns = duration;
        }
    }
    result.clock_pair_median_ns = summarize_latency(pairs).p50_ns;
    return result;
}
} // namespace apex
