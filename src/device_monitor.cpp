#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>

#include "device_monitor.h"
#include "usb_structures.h"

// 全局调试标志 (在 main.cpp 中定义)
extern bool g_debug;

// 调试打印宏
#define DEBUG_PRINT(fmt, ...) \
    do { if (g_debug) fprintf(stderr, "[DeviceMonitor] " fmt "\n", ##__VA_ARGS__); } while(0)

#define DEBUG_ERROR(fmt, ...) \
    do { if (g_debug) fprintf(stderr, "[DeviceMonitor] ERROR: " fmt "\n", ##__VA_ARGS__); } while(0)

// 检查设备是否已挂载
bool DeviceMonitor::is_device_attached() {
    // 1. 优先检查缓存的 busid
    if (!last_found_busid.empty() && last_found_busid != "0-0") {
        if (check_device_match(last_found_busid)) {
            DEBUG_PRINT("Device found via cached busid: %s", last_found_busid.c_str());
            return true;
        }
        // 缓存的 busid 无效，清空缓存
        DEBUG_PRINT("Cached busid %s is no longer valid, searching...", last_found_busid.c_str());
        last_found_busid.clear();
    }

    std::string status_path = "/sys/devices/platform/vhci_hcd.0/status";
    std::ifstream status_file(status_path);

    if (!status_file.is_open()) {
        DEBUG_ERROR("Failed to open %s", status_path.c_str());
        return false;
    }

    std::string line;
    std::vector<std::string> local_busids;

    // 读取状态文件，获取所有 local_busid
    // 文件格式：hub port sta spd dev sockfd local_busid
    // 示例：hs  0000 006 002 00010085 000003 11-1
    //       hs  0001 004 000 00000000 000000 0-0
    while (std::getline(status_file, line)) {
        // 跳过空行和标题行
        if (line.empty() || line.find("hub") != std::string::npos) {
            continue;
        }

        // 解析行内容，提取 local_busid
        std::istringstream iss(line);
        std::string hub, port, sta, spd, dev, sockfd, local_busid;

        iss >> hub >> port >> sta >> spd >> dev >> sockfd >> local_busid;

        // 过滤出 local_busid 不为 "0-0" 的设备（已挂载的设备）
        if (!local_busid.empty() && local_busid != "0-0") {
            local_busids.push_back(local_busid);
        }
    }

    status_file.close();

    // 检查每个已挂载的设备
    for (const auto& busid : local_busids) {
        if (check_device_match(busid)) {
            // 缓存找到的 busid
            last_found_busid = busid;
            DEBUG_PRINT("Device found via global search, cached busid: %s", last_found_busid.c_str());
            return true;
        }
    }

    return false;
}

// 检查指定 busid 的设备是否匹配配置
bool DeviceMonitor::check_device_match(const std::string& busid) {
    std::string base_path = "/sys/bus/usb/devices/" + busid;

    // 读取 vendor id
    std::string vendor_path = base_path + "/idVendor";
    std::ifstream vendor_file(vendor_path);
    if (!vendor_file.is_open()) {
        return false;
    }

    std::string vendor_str;
    std::getline(vendor_file, vendor_str);
    vendor_file.close();

    uint16_t device_vendor_id = static_cast<uint16_t>(std::stoul(vendor_str, nullptr, 16));
    if (device_vendor_id != vendor_id) {
        return false;
    }

    // 读取 product id
    std::string product_path = base_path + "/idProduct";
    std::ifstream product_file(product_path);
    if (!product_file.is_open()) {
        return false;
    }

    std::string product_str;
    std::getline(product_file, product_str);
    product_file.close();

    uint16_t device_product_id = static_cast<uint16_t>(std::stoul(product_str, nullptr, 16));
    if (device_product_id != product_id) {
        return false;
    }

    // 读取 serial number
    std::string serial_path = base_path + "/serial";
    std::ifstream serial_file(serial_path);
    if (!serial_file.is_open()) {
        return false;
    }

    std::string device_serial;
    std::getline(serial_file, device_serial);
    serial_file.close();

    // 匹配序列号
    if (device_serial != serial_number) {
        return false;
    }

    return true;
}

// 执行 usbip attach 命令
bool DeviceMonitor::attach_device() {
    std::string cmd = "usbip --tcp-port " + std::to_string(port) + " attach -r " + host + " -b " + bus_id;
    DEBUG_PRINT("Executing: %s", cmd.c_str());

    int result = system(cmd.c_str());
    if (result != 0) {
        DEBUG_ERROR("Failed to attach device, exit code: %d", WEXITSTATUS(result));
        return false;
    }

    DEBUG_PRINT("Device attached successfully");
    return true;
}

// 监控线程函数
void DeviceMonitor::monitor_loop() {
    // 等待 4 秒，确保系统有足够的时间初始化
    int wait_time = 4;
    int sleep_count = 0;

    DEBUG_PRINT("Starting monitor thread for %s:%s", host.c_str(), bus_id.c_str());

    while (running.load()) {
        if (sleep_count < wait_time) {
            sleep_count += 4;
            sleep(4);
            continue;
        } else {
            sleep_count = 0;
        }

        if (is_device_attached()) {
            if (wait_time < 256) {
                wait_time += wait_time;
            }
        } else {
            DEBUG_PRINT("Device not attached, attempting to attach...");
            if (attach_device()) {
                DEBUG_PRINT("Waiting 5 seconds for device to attach...");
                sleep(5);

                if (is_device_attached()) {
                    DEBUG_PRINT("Device attached successfully");
                } else {
                    DEBUG_ERROR("Device still not attached after 5 seconds");
                }
            }
            wait_time = 16;
        }
    }

    DEBUG_PRINT("Monitor thread stopped");
}

// 构造函数
DeviceMonitor::DeviceMonitor(const std::string& host, int port, const std::string& bus_id,
                             uint16_t vendor_id, uint16_t product_id, const std::string& serial_number)
    : host(host), port(port), bus_id(bus_id), vendor_id(vendor_id), product_id(product_id),
      serial_number(serial_number), running(false) {
}

// 析构函数
DeviceMonitor::~DeviceMonitor() {
    stop();
}

// 启动监控
void DeviceMonitor::start() {
    if (running.load()) {
        return;
    }

    running.store(true);
    monitor_thread = std::thread(&DeviceMonitor::monitor_loop, this);
}

// 停止监控
void DeviceMonitor::stop() {
    if (!running.load()) {
        return;
    }

    running.store(false);
    if (monitor_thread.joinable()) {
        monitor_thread.join();
    }
}

// 是否正在运行
bool DeviceMonitor::is_running() const {
    return running.load();
}
