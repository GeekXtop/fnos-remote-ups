#ifndef DEVICE_MONITOR_H
#define DEVICE_MONITOR_H

#include <string>
#include <thread>
#include <atomic>

// USB 设备监控器类
class DeviceMonitor {
private:
    std::string host;
    int port;
    std::string bus_id;
    uint16_t vendor_id;
    uint16_t product_id;
    std::string serial_number;
    std::thread monitor_thread;
    std::atomic<bool> running;
    std::string last_found_busid;  // 缓存最后一次搜索到的 local_busid

    // 检查设备是否已挂载
    bool is_device_attached();

    // 检查指定 busid 的设备是否匹配配置
    bool check_device_match(const std::string& busid);

    // 执行 usbip attach 命令
    bool attach_device();

    // 监控线程函数
    void monitor_loop();

public:
    DeviceMonitor(const std::string& host, int port, const std::string& bus_id,
                  uint16_t vendor_id, uint16_t product_id, const std::string& serial_number);
    ~DeviceMonitor();

    // 启动监控
    void start();

    // 停止监控
    void stop();

    // 是否正在运行
    bool is_running() const;
};

#endif // DEVICE_MONITOR_H
