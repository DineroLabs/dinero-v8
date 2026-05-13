/**
 * CPU Stats RPC Methods - Phase E.3.1
 *
 * Read-only diagnostic endpoints for CPU budget monitoring.
 * Exposes internal counters from CPUBudgetMonitor for operator visibility.
 *
 * Design Principles:
 * - Read-only (no control or tuning)
 * - Non-consensus (diagnostic only)
 * - No changes to validation paths
 * - Pulls from existing atomic counters
 *
 * Endpoints:
 * - node.getcpustats: Detailed CPU budget statistics
 * - node.getresourcepressure: Aggregate resource health status
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "consensus/cpu_budget_monitor.h"
#include "daemon/mining_safety_gates.h"
#include "storage/disk_space_monitor.h"
#include "common/logger.h"
#include <cstring>
#include <memory>
#include <string>

#if defined(__APPLE__) && !defined(IOS_BUILD)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#define DINERO_HAS_MACOS_GPU_TELEMETRY 1
#endif

// ═══════════════════════════════════════════════════════════════
// PHASE E.3.1: CPU STATS RPC HANDLERS (Read-Only Observability)
// ═══════════════════════════════════════════════════════════════

namespace {

struct GpuTelemetrySnapshot {
    bool supported = false;
    std::string backend;
    std::string model;
    int64_t coreCount = 0;
    double deviceUtilizationPercent = 0.0;
    double rendererUtilizationPercent = 0.0;
    double tilerUtilizationPercent = 0.0;
    int64_t memoryInUseBytes = 0;
    int64_t memoryDriverBytes = 0;
    int64_t memoryAllocatedBytes = 0;
    bool temperatureAvailable = false;
    double temperatureC = 0.0;
    bool fanAvailable = false;
    int64_t fanRpm = 0;
    std::string source;
    std::string unavailabilityReason;
};

#if defined(DINERO_HAS_MACOS_GPU_TELEMETRY)
mach_port_t DineroIOMainPort() {
    return kIOMainPortDefault;
}

std::string CFStringToStdString(CFStringRef value) {
    if (!value) {
        return {};
    }

    const char* direct = CFStringGetCStringPtr(value, kCFStringEncodingUTF8);
    if (direct) {
        return direct;
    }

    const CFIndex len = CFStringGetLength(value);
    const CFIndex maxSize = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string result(static_cast<size_t>(maxSize), '\0');
    if (CFStringGetCString(value, result.data(), maxSize, kCFStringEncodingUTF8)) {
        result.resize(std::strlen(result.c_str()));
        return result;
    }
    return {};
}

bool CFNumberToDouble(CFTypeRef value, double* out) {
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID() || !out) {
        return false;
    }

    if (CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, out)) {
        return true;
    }

    long long integerValue = 0;
    if (CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &integerValue)) {
        *out = static_cast<double>(integerValue);
        return true;
    }

    return false;
}

bool CFNumberToInt64(CFTypeRef value, int64_t* out) {
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID() || !out) {
        return false;
    }

    long long integerValue = 0;
    if (CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &integerValue)) {
        *out = static_cast<int64_t>(integerValue);
        return true;
    }

    double doubleValue = 0.0;
    if (CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &doubleValue)) {
        *out = static_cast<int64_t>(doubleValue);
        return true;
    }

    return false;
}

bool CFDictionaryReadDouble(CFDictionaryRef dict, const char* key, double* out) {
    if (!dict || !key || !out) {
        return false;
    }
    CFStringRef cfKey = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!cfKey) {
        return false;
    }
    CFTypeRef value = CFDictionaryGetValue(dict, cfKey);
    CFRelease(cfKey);
    return CFNumberToDouble(value, out);
}

bool CFDictionaryReadInt64(CFDictionaryRef dict, const char* key, int64_t* out) {
    if (!dict || !key || !out) {
        return false;
    }
    CFStringRef cfKey = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!cfKey) {
        return false;
    }
    CFTypeRef value = CFDictionaryGetValue(dict, cfKey);
    CFRelease(cfKey);
    return CFNumberToInt64(value, out);
}

bool CFDictionaryReadString(CFDictionaryRef dict, const char* key, std::string* out) {
    if (!dict || !key || !out) {
        return false;
    }
    CFStringRef cfKey = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!cfKey) {
        return false;
    }
    CFTypeRef value = CFDictionaryGetValue(dict, cfKey);
    CFRelease(cfKey);
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) {
        return false;
    }
    *out = CFStringToStdString(static_cast<CFStringRef>(value));
    return true;
}

GpuTelemetrySnapshot ReadMacOSGpuTelemetry() {
    GpuTelemetrySnapshot snapshot;
    snapshot.backend = "metal";
    snapshot.source = "macOS AGXAccelerator PerformanceStatistics";
    snapshot.unavailabilityReason = "GPU temperature/fan telemetry is not exposed via public Apple/Metal APIs";

    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(
        DineroIOMainPort(), IOServiceMatching("AGXAccelerator"), &iterator);
    if (kr != KERN_SUCCESS || iterator == IO_OBJECT_NULL) {
        snapshot.unavailabilityReason = "AGXAccelerator service not found";
        return snapshot;
    }

    io_service_t service = IOIteratorNext(iterator);
    if (service == IO_OBJECT_NULL) {
        IOObjectRelease(iterator);
        snapshot.unavailabilityReason = "No Apple GPU accelerator service available";
        return snapshot;
    }

    CFMutableDictionaryRef properties = nullptr;
    kr = IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, kNilOptions);
    IOObjectRelease(service);
    IOObjectRelease(iterator);
    if (kr != KERN_SUCCESS || !properties) {
        snapshot.unavailabilityReason = "Unable to read Apple GPU registry properties";
        return snapshot;
    }

    CFDictionaryRef stats = nullptr;
    if (CFTypeRef rawStats = CFDictionaryGetValue(properties, CFSTR("PerformanceStatistics"));
        rawStats && CFGetTypeID(rawStats) == CFDictionaryGetTypeID()) {
        stats = static_cast<CFDictionaryRef>(rawStats);
        snapshot.supported = true;
        CFDictionaryReadDouble(stats, "Device Utilization %", &snapshot.deviceUtilizationPercent);
        CFDictionaryReadDouble(stats, "Renderer Utilization %", &snapshot.rendererUtilizationPercent);
        CFDictionaryReadDouble(stats, "Tiler Utilization %", &snapshot.tilerUtilizationPercent);
        CFDictionaryReadInt64(stats, "In use system memory", &snapshot.memoryInUseBytes);
        CFDictionaryReadInt64(stats, "In use system memory (driver)", &snapshot.memoryDriverBytes);
        CFDictionaryReadInt64(stats, "Alloc system memory", &snapshot.memoryAllocatedBytes);
    }

    CFDictionaryReadString(properties, "model", &snapshot.model);
    CFDictionaryReadInt64(properties, "gpu-core-count", &snapshot.coreCount);

    if (snapshot.model.empty()) {
        snapshot.unavailabilityReason = "GPU model unavailable from registry";
    }

    if (snapshot.unavailabilityReason.empty()) {
        snapshot.unavailabilityReason =
            "Apple public GPU telemetry exposes load and memory, but not GPU temperature or fan RPM";
    }

    CFRelease(properties);
    return snapshot;
}
#endif

GpuTelemetrySnapshot ReadGpuTelemetry() {
#if defined(DINERO_HAS_MACOS_GPU_TELEMETRY)
    return ReadMacOSGpuTelemetry();
#else
    GpuTelemetrySnapshot snapshot;
    snapshot.unavailabilityReason = "GPU telemetry is not implemented on this platform";
    return snapshot;
#endif
}

din::Json BuildGpuTelemetryJson() {
    const auto gpu = ReadGpuTelemetry();
    din::Json result;
    result["supported"] = gpu.supported;
    result["backend"] = gpu.backend;
    result["source"] = gpu.source;
    result["temp_available"] = gpu.temperatureAvailable;
    result["fan_available"] = gpu.fanAvailable;
    if (!gpu.model.empty()) {
        result["model"] = gpu.model;
    }
    if (gpu.coreCount > 0) {
        result["core_count"] = static_cast<int>(gpu.coreCount);
    }
    if (gpu.supported) {
        result["device_utilization_percent"] = gpu.deviceUtilizationPercent;
        result["renderer_utilization_percent"] = gpu.rendererUtilizationPercent;
        result["tiler_utilization_percent"] = gpu.tilerUtilizationPercent;
        result["memory_in_use_bytes"] = static_cast<Json::LargestInt>(gpu.memoryInUseBytes);
        result["memory_driver_bytes"] = static_cast<Json::LargestInt>(gpu.memoryDriverBytes);
        result["memory_allocated_bytes"] = static_cast<Json::LargestInt>(gpu.memoryAllocatedBytes);
    }
    if (gpu.temperatureAvailable) {
        result["temp_c"] = gpu.temperatureC;
    } else {
        result["temp_reason"] = gpu.unavailabilityReason;
    }
    if (gpu.fanAvailable) {
        result["fan_rpm"] = static_cast<int>(gpu.fanRpm);
    } else {
        result["fan_reason"] = gpu.unavailabilityReason;
    }
    if (!gpu.supported && !gpu.unavailabilityReason.empty()) {
        result["reason"] = gpu.unavailabilityReason;
    }
    return result;
}

}  // namespace

/**
 * node.getcpustats - Get detailed CPU budget statistics
 *
 * Returns CPU budget monitoring data from CPUBudgetMonitor:
 * - Validation time tracking (scripts, blocks, signatures)
 * - Timeout configuration
 * - Violation counts
 * - Budget status
 *
 * Example response:
 * {
 *   "script_validation": {
 *     "budget_ms": 100,
 *     "total_validated": 1234,
 *     "total_time_ms": 5678,
 *     "timeouts": 2,
 *     "timeout_rate_percent": 0.16
 *   },
 *   "block_validation": {
 *     "budget_ms": 30000,
 *     "total_validated": 45,
 *     "total_time_ms": 12345,
 *     "timeouts": 0,
 *     "timeout_rate_percent": 0.0
 *   },
 *   "signature_verification": {
 *     "budget_ms": 50,
 *     "total_verified": 9876,
 *     "total_time_ms": 3456,
 *     "timeouts": 1,
 *     "timeout_rate_percent": 0.01
 *   },
 *   "cpu_load_percent": 42.5,
 *   "status": "OK"
 * }
 */
din::Json rpc_getcpustats(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Phase E.3.1: Pull real CPU budget data from DaemonContext
    if (ctx.daemon && ctx.daemon->cpu_monitor) {
        auto usage = ctx.daemon->cpu_monitor->getCPUUsage();
        const auto& config = ctx.daemon->cpu_monitor->getConfig();

        // Script validation stats
        din::Json script_stats;
        script_stats["budget_ms"] = static_cast<int>(config.max_script_validation_ms);
        script_stats["total_validated"] = static_cast<int>(usage.scripts_validated);
        script_stats["total_time_ms"] = static_cast<int>(usage.script_validation_time_ms);
        script_stats["timeouts"] = static_cast<int>(usage.script_timeouts);

        // Calculate timeout rate percentage
        if (usage.scripts_validated > 0) {
            script_stats["timeout_rate_percent"] =
                (static_cast<double>(usage.script_timeouts) / usage.scripts_validated) * 100.0;
        } else {
            script_stats["timeout_rate_percent"] = 0.0;
        }
        result["script_validation"] = script_stats;

        // Block validation stats
        din::Json block_stats;
        block_stats["budget_ms"] = static_cast<int>(config.max_block_validation_ms);
        block_stats["total_validated"] = static_cast<int>(usage.blocks_validated);
        block_stats["total_time_ms"] = static_cast<int>(usage.block_validation_time_ms);
        block_stats["timeouts"] = static_cast<int>(usage.block_timeouts);

        // Calculate timeout rate percentage
        if (usage.blocks_validated > 0) {
            block_stats["timeout_rate_percent"] =
                (static_cast<double>(usage.block_timeouts) / usage.blocks_validated) * 100.0;
        } else {
            block_stats["timeout_rate_percent"] = 0.0;
        }
        result["block_validation"] = block_stats;

        // Signature verification stats
        din::Json sig_stats;
        sig_stats["budget_ms"] = static_cast<int>(config.max_signature_verification_ms);
        sig_stats["total_verified"] = static_cast<int>(usage.signatures_verified);
        sig_stats["total_time_ms"] = static_cast<int>(usage.signature_validation_time_ms);
        sig_stats["timeouts"] = 0;  // Signatures tracked via script timeouts
        sig_stats["timeout_rate_percent"] = 0.0;
        result["signature_verification"] = sig_stats;

        // CPU load (platform-specific, may not be available)
        result["cpu_load_percent"] = usage.cpu_load_percent;

        // Overall status (from CPUBudgetStatus enum)
        using dinero::consensus::CPUBudgetStatus;
        switch (usage.status) {
            case CPUBudgetStatus::OK:
                result["status"] = "OK";
                break;
            case CPUBudgetStatus::WARNING:
                result["status"] = "WARNING";
                break;
            case CPUBudgetStatus::CRITICAL:
                result["status"] = "CRITICAL";
                break;
            case CPUBudgetStatus::EXHAUSTED:
                result["status"] = "EXHAUSTED";
                break;
            case CPUBudgetStatus::ERROR:
            default:
                result["status"] = "ERROR";
                break;
        }
    } else {
        // Fallback: CPU monitor not initialized (return safe defaults)
        din::Json script_stats;
        script_stats["budget_ms"] = 100;
        script_stats["total_validated"] = 0;
        script_stats["total_time_ms"] = 0;
        script_stats["timeouts"] = 0;
        script_stats["timeout_rate_percent"] = 0.0;
        result["script_validation"] = script_stats;

        din::Json block_stats;
        block_stats["budget_ms"] = 30000;
        block_stats["total_validated"] = 0;
        block_stats["total_time_ms"] = 0;
        block_stats["timeouts"] = 0;
        block_stats["timeout_rate_percent"] = 0.0;
        result["block_validation"] = block_stats;

        din::Json sig_stats;
        sig_stats["budget_ms"] = 50;
        sig_stats["total_verified"] = 0;
        sig_stats["total_time_ms"] = 0;
        sig_stats["timeouts"] = 0;
        sig_stats["timeout_rate_percent"] = 0.0;
        result["signature_verification"] = sig_stats;

        result["cpu_load_percent"] = 0.0;
        result["status"] = "OK";
    }

    const auto thermal = MiningSafetyGates::CheckThermalStatus();
    result["cpu_temp_c"] = thermal.cpuTemp;
    result["thermal_throttling"] = thermal.thermalThrottling;
    result["thermal_safe"] = thermal.safeToMine;
    if (!thermal.thermalReason.empty()) {
        result["thermal_reason"] = thermal.thermalReason;
    }

    const auto battery = MiningSafetyGates::CheckBatteryStatus();
    result["on_battery"] = battery.onBattery;
    result["battery_percent"] = battery.batteryPercent;
    result["low_battery"] = battery.lowBattery;
    result["battery_safe"] = battery.safeToMine;
    if (!battery.batteryReason.empty()) {
        result["battery_reason"] = battery.batteryReason;
    }
    result["gpu"] = BuildGpuTelemetryJson();

    return result;
}

/**
 * node.getresourcepressure - Get aggregate resource health status
 *
 * Provides unified view of all resource exhaustion monitors:
 * - CPU (from CPUBudgetMonitor)
 * - Memory (from MemoryMonitor - Phase E.2.a)
 * - Disk (from DiskSpaceMonitor - Phase E.2.b)
 * - Network (from NetworkLimitsMonitor - Phase E.2.c)
 *
 * Returns simple health status for each resource category.
 *
 * Example response:
 * {
 *   "cpu": "OK",
 *   "memory": "OK",
 *   "disk": "WARNING",
 *   "network": "OK",
 *   "overall": "WARNING"
 * }
 *
 * Status values:
 * - "OK": Resource usage within normal limits
 * - "WARNING": Approaching resource limits (5-10% violation rate)
 * - "CRITICAL": Near resource limits (10-20% violation rate)
 * - "EXHAUSTED": Resource limits exceeded (>20% violation rate)
 * - "ERROR": Monitoring error or unavailable
 */
din::Json rpc_getresourcepressure(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Phase E.3.1: Pull real CPU status from DaemonContext
    std::string cpu_status = "OK";
    if (ctx.daemon && ctx.daemon->cpu_monitor) {
        auto usage = ctx.daemon->cpu_monitor->getCPUUsage();
        using dinero::consensus::CPUBudgetStatus;
        switch (usage.status) {
            case CPUBudgetStatus::OK:
                cpu_status = "OK";
                break;
            case CPUBudgetStatus::WARNING:
                cpu_status = "WARNING";
                break;
            case CPUBudgetStatus::CRITICAL:
                cpu_status = "CRITICAL";
                break;
            case CPUBudgetStatus::EXHAUSTED:
                cpu_status = "EXHAUSTED";
                break;
            case CPUBudgetStatus::ERROR:
            default:
                cpu_status = "ERROR";
                break;
        }
    }
    result["cpu"] = cpu_status;

    // Memory status (from MemoryMonitor - Phase E.2.a)
    // NOTE: Memory monitoring integrated into Mempool via MemoryStats
    // Future: expose via mempool->GetMemoryUsage() if needed
    result["memory"] = "OK";

    // Disk status (from DiskSpaceMonitor - Phase E.2.b)
    std::string disk_status = "OK";
    if (ctx.daemon && ctx.daemon->disk_monitor) {
        auto disk_info = ctx.daemon->disk_monitor->checkDiskSpace();
        using dinero::storage::DiskSpaceStatus;
        switch (disk_info.status) {
            case DiskSpaceStatus::OK:
                disk_status = "OK";
                break;
            case DiskSpaceStatus::LOW:
                disk_status = "WARNING";
                break;
            case DiskSpaceStatus::CRITICAL:
                disk_status = "CRITICAL";
                break;
            case DiskSpaceStatus::FULL:
                disk_status = "EXHAUSTED";
                break;
            case DiskSpaceStatus::ERROR:
            default:
                disk_status = "ERROR";
                break;
        }
    }
    result["disk"] = disk_status;

    // Network status (from NetworkLimitsMonitor - Phase E.2.c)
    // NOTE: NetworkLimitsMonitor requires P2P internal components
    // Future: initialize after P2PService is available
    result["network"] = "OK";

    // Overall status (worst of all resources)
    // Aggregate CPU and disk status (network not yet wired)
    std::string overall_status = cpu_status;
    if (disk_status == "EXHAUSTED" || overall_status == "EXHAUSTED") {
        overall_status = "EXHAUSTED";
    } else if (disk_status == "CRITICAL" || overall_status == "CRITICAL") {
        overall_status = "CRITICAL";
    } else if (disk_status == "WARNING" || overall_status == "WARNING") {
        overall_status = "WARNING";
    } else if (disk_status == "ERROR" || overall_status == "ERROR") {
        overall_status = "ERROR";
    }
    result["overall"] = overall_status;

    return result;
}

/**
 * node.getdiskstats - Get detailed disk space statistics
 *
 * Returns disk space monitoring data from DiskSpaceMonitor:
 * - Total/available/used disk space
 * - Usage percentages
 * - Status (OK/LOW/CRITICAL/FULL/ERROR)
 * - Path being monitored
 *
 * Example response:
 * {
 *   "total_bytes": 1000000000000,
 *   "available_bytes": 500000000000,
 *   "free_bytes": 520000000000,
 *   "used_bytes": 480000000000,
 *   "usage_percent": 48.0,
 *   "available_percent": 52.0,
 *   "status": "OK",
 *   "path": "/Users/user/.dinerocoin"
 * }
 *
 * Phase X.2: Disk monitoring integration for GUI
 */
din::Json rpc_getdiskstats(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Phase X.2: Pull real disk space data from DaemonContext
    if (ctx.daemon && ctx.daemon->disk_monitor) {
        auto disk_info = ctx.daemon->disk_monitor->checkDiskSpace();

        result["total_bytes"] = static_cast<int64_t>(disk_info.total_bytes);
        result["available_bytes"] = static_cast<int64_t>(disk_info.available_bytes);
        result["free_bytes"] = static_cast<int64_t>(disk_info.free_bytes);
        result["used_bytes"] = static_cast<int64_t>(disk_info.used_bytes);
        result["usage_percent"] = disk_info.usage_percent;
        result["available_percent"] = disk_info.available_percent;
        result["path"] = disk_info.path;

        using dinero::storage::DiskSpaceStatus;
        switch (disk_info.status) {
            case DiskSpaceStatus::OK:
                result["status"] = "OK";
                break;
            case DiskSpaceStatus::LOW:
                result["status"] = "LOW";
                break;
            case DiskSpaceStatus::CRITICAL:
                result["status"] = "CRITICAL";
                break;
            case DiskSpaceStatus::FULL:
                result["status"] = "FULL";
                break;
            case DiskSpaceStatus::ERROR:
            default:
                result["status"] = "ERROR";
                break;
        }
    } else {
        // Fallback: Disk monitor not initialized (return safe defaults)
        result["total_bytes"] = 0;
        result["available_bytes"] = 0;
        result["free_bytes"] = 0;
        result["used_bytes"] = 0;
        result["usage_percent"] = 0.0;
        result["available_percent"] = 100.0;
        result["status"] = "OK";
        result["path"] = "";
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// RPC REGISTRATION (Modern vNext Pattern)
// ═══════════════════════════════════════════════════════════════

/**
 * Register CPU stats RPC methods
 *
 * Called during daemon initialization to register handlers.
 */
void register_cpu_stats_rpc_methods(RpcRegistry& registry) {
    // node.getcpustats - Detailed CPU budget statistics
    RpcMethodMeta getcpustats_meta;
    getcpustats_meta.name = "getcpustats";
    getcpustats_meta.ns = "node";
    getcpustats_meta.description = "Get detailed CPU budget statistics (validation times, timeouts, status)";
    getcpustats_meta.result.type = "object";
    getcpustats_meta.result.desc = "CPU budget statistics";

    registry.registerHandler("node.getcpustats", rpc_getcpustats, getcpustats_meta, "Phase E.3.1");

    // node.getresourcepressure - Aggregate resource health
    RpcMethodMeta getresourcepressure_meta;
    getresourcepressure_meta.name = "getresourcepressure";
    getresourcepressure_meta.ns = "node";
    getresourcepressure_meta.description = "Get aggregate resource health status (CPU, memory, disk, network)";
    getresourcepressure_meta.result.type = "object";
    getresourcepressure_meta.result.desc = "Resource health status";

    registry.registerHandler("node.getresourcepressure", rpc_getresourcepressure, getresourcepressure_meta, "Phase E.3.1");

    // node.getdiskstats - Detailed disk space statistics
    RpcMethodMeta getdiskstats_meta;
    getdiskstats_meta.name = "getdiskstats";
    getdiskstats_meta.ns = "node";
    getdiskstats_meta.description = "Get detailed disk space statistics (total, used, available, status)";
    getdiskstats_meta.result.type = "object";
    getdiskstats_meta.result.desc = "Disk space statistics";

    registry.registerHandler("node.getdiskstats", rpc_getdiskstats, getdiskstats_meta, "Phase X.2");
}
