#ifndef UPS_DEVICE_H
#define UPS_DEVICE_H

#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>
#include <map>
#include <string>
#include <chrono>

#include "nut_client.h"
#include "usb_structures.h"

// UPS设备类
class UPSDevice {
private:
    std::mutex status_mutex;
    ups_status current_status;
    std::atomic<bool> running;
    std::thread status_thread;

    // NUT客户端相关
    std::unique_ptr<NUTClient> nut_client;
    std::string last_status;
    std::chrono::time_point<std::chrono::steady_clock> last_full_query_time;
    std::chrono::seconds full_query_interval;
    std::string manufacturer;
    std::string product;

    // USB描述符数据
    std::vector<uint8_t> device_descriptor;
    std::vector<uint8_t> config_descriptor;
    std::vector<uint8_t> interface_descriptor;
    std::vector<uint8_t> hid_descriptor;
    std::vector<uint8_t> endpoint_descriptor;
    std::vector<uint8_t> report_descriptor;
    std::vector<uint8_t> string_descriptors[MAX_STRING_COUNT];

    // 生成USB字符串描述符的辅助函数
    std::vector<uint8_t> create_string_descriptor(const std::string& str);

    void init_descriptors();
    void update_status_loop();
    void parse_nut_status(const std::string& nut_status);
    std::map<std::string, std::string> get_status_from_nut();

public:
    UPSDevice(const std::string& ups_identifier,
              const std::string& manufacturer = DEFAULT_DEVICE_MANUFACTURER,
              const std::string& product = DEFAULT_DEVICE_PRODUCT);
    ~UPSDevice();

    void start();
    void stop();
    ups_status get_status();
    int get_report(uint8_t report_type, uint8_t report_id, uint8_t* response_data, uint16_t response_length);

    // USB请求处理
    int handle_control_request(const struct usb_setup_packet* setup_packet, uint8_t* response_data);
    int handle_interrupt_request(const struct usb_setup_packet* setup_packet, uint8_t* response_data);

    // 获取描述符
    const std::vector<uint8_t>& get_device_descriptor() const { return device_descriptor; }
    const std::vector<uint8_t>& get_config_descriptor() const { return config_descriptor; }
    const std::vector<uint8_t>& get_hid_descriptor() const { return hid_descriptor; }
    const std::vector<uint8_t>& get_report_descriptor() const { return report_descriptor; }
    const std::vector<uint8_t>& get_string_descriptor(int index) const;
};

#endif // UPS_DEVICE_H
