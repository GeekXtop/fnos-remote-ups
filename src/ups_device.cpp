#include <iostream>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <memory>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <ctime>
#include <algorithm>
#include <map>
#include <string>
#include <cmath>

#include "ups_device.h"
#include "usb_structures.h"
#include "hid_report.h"

// 引入调试宏
extern bool g_debug;
#define DEBUG_PRINT(fmt, ...) \
    do { if (g_debug) fprintf(stderr, fmt, ##__VA_ARGS__); } while(0)

static uint16_t clamp_double_to_u16(double value) {
    if (std::isnan(value) || value <= 0.0) {
        return 0;
    }
    if (value >= 65535.0) {
        return 65535;
    }
    return static_cast<uint16_t>(std::lround(value));
}

static int16_t clamp_double_to_i16(double value) {
    if (std::isnan(value)) {
        return 0;
    }
    if (value <= -32768.0) {
        return -32768;
    }
    if (value >= 32767.0) {
        return 32767;
    }
    return static_cast<int16_t>(std::lround(value));
}

static uint8_t percent_to_u8(uint16_t value) {
    return static_cast<uint8_t>(std::min<uint16_t>(value, 100));
}

static uint16_t parse_scaled_u16(const std::string& value, double scale) {
    return clamp_double_to_u16(std::stod(value) * scale);
}

static int16_t parse_scaled_i16(const std::string& value, double scale) {
    return clamp_double_to_i16(std::stod(value) * scale);
}

static uint16_t openups_temperature_raw(int16_t centi_celsius) {
    static const uint16_t therm_tbl[] = {
        0x31, 0x40, 0x53, 0x68, 0x82, 0xA0, 0xC3, 0xE9,
        0x113, 0x13F, 0x16E, 0x19F, 0x1CF, 0x200, 0x22F,
        0x25C, 0x286, 0x2AE, 0x2D3, 0x2F4, 0x312, 0x32D,
        0x345, 0x35A, 0x36D, 0x37E, 0x38C, 0x399, 0x3A5,
        0x3AF, 0x3B7, 0x3BF, 0x3C6, 0x3CC
    };

    double celsius = centi_celsius / 100.0;
    if (celsius <= -40.0) {
        return therm_tbl[0];
    }
    if (celsius >= 125.0) {
        return therm_tbl[(sizeof(therm_tbl) / sizeof(therm_tbl[0])) - 1];
    }

    double pos = (celsius + 40.0) / 5.0;
    size_t idx = static_cast<size_t>(std::floor(pos));
    size_t last = (sizeof(therm_tbl) / sizeof(therm_tbl[0])) - 1;
    if (idx >= last) {
        return therm_tbl[last];
    }

    double fraction = pos - idx;
    double raw = therm_tbl[idx] + fraction * (therm_tbl[idx + 1] - therm_tbl[idx]);
    return clamp_double_to_u16(raw);
}

// 全局设备信息
struct usbip_device global_devinfo = {
    "/sys/devices/usb1/1-1",  // path
    "1-1",                    // busid
    htonl(1),                 // busnum
    htonl(0x10),              // devnum
    htonl(USB_SPEED_FULL),    // speed
    htons(DEFAULT_DEVICE_VENDOR_ID),  // idVendor
    htons(DEFAULT_DEVICE_PRODUCT_ID), // idProduct
    htons(0x0100),            // bcdDevice
    0x03,                     // bDeviceClass
    0x00,                     // bDeviceSubClass
    0x00,                     // bDeviceProtocol
    0,                        // bConfigurationValue
    1,                        // bNumConfigurations
    1                         // bNumInterfaces
};

struct usbip_interface global_intf = {
    0x03,  // bInterfaceClass: HID
    0x00,  // bInterfaceSubClass
    0x00,  // bInterfaceProtocol
    0x00   // padding
};

// UPS设备实现
UPSDevice::UPSDevice(const std::string& ups_identifier,
                     const std::string& manufacturer,
                     const std::string& product,
                     uint16_t vendor_id,
                     uint16_t product_id)
    : running(false),
      last_full_query_time(std::chrono::steady_clock::now()),
      full_query_interval(60),
      manufacturer(manufacturer),
      product(product),
      vendor_id(vendor_id),
      product_id(product_id) {
    // 初始化UPS状态
    std::memset(&current_status, 0, sizeof(current_status));
    current_status.power_summary.ac_present = UPS_AC_PRESENT;

    current_status.input_voltage = 1237;    // 12.37V输入
    current_status.output_voltage = 1219;   // 12.19V输出
    current_status.battery_voltage = 1361;  // 13.61V电池
    current_status.input_current = 1340;    // 1.340A输入
    current_status.output_current = 1360;   // 1.360A输出
    current_status.battery_current = 18;    // 0.018A电池
    current_status.battery_temperature = 850; // 8.50C
    current_status.battery_capacity = 100;
    current_status.battery_charge_low = 10;
    current_status.battery_charge_warning = 5;
    current_status.battery_charge = 100;  // 100%电量
    current_status.load_percent = 25;     // 25%负载
    current_status.runtime = 11300;
    current_status.runtime_low = 300;
    current_status.input_frequency = 500;

    try {
        nut_client = std::unique_ptr<NUTClient>(new NUTClient(ups_identifier));
        DEBUG_PRINT("NUT client initialized for UPS: %s\n", ups_identifier.c_str());
    } catch (const std::exception& e) {
        DEBUG_PRINT("Failed to initialize NUT client: %s\n", e.what());
    }

    init_descriptors();
}

UPSDevice::~UPSDevice() {
    stop();
}

void UPSDevice::init_descriptors() {
    // 字符串描述符
    string_descriptors[0] = { 0x04, USB_DT_STRING, 0x09, 0x04 };    // Language ID English
    string_descriptors[1] = create_string_descriptor(manufacturer); // Manufacturer
    string_descriptors[2] = create_string_descriptor(product);   // Product
    string_descriptors[3] = create_string_descriptor(DEVICE_SERIAL_NUMBER);   // Serial Number
    string_descriptors[4] = create_string_descriptor(DEVICE_OEM_INFO);        // OEMInfomation
    string_descriptors[5] = create_string_descriptor(DEVICE_BATTERY_TYPE);     // BatteryType

    // 设备描述符
    struct usb_device_descriptor dev_desc = {
        .bLength = sizeof(dev_desc),
        .bDescriptorType = USB_DT_DEVICE,
        .bcdUSB = 0x0110,
        .bDeviceClass = 0x03,
        .bDeviceSubClass = 0x00,
        .bDeviceProtocol = 0x00,
        .bMaxPacketSize0 = 64,
        .idVendor = vendor_id,
        .idProduct = product_id,
        .bcdDevice = 0x0100,
        .iManufacturer = 1,
        .iProduct = 2,
        .iSerialNumber = 3,
        .bNumConfigurations = 1
    };

    // 配置描述符
    struct usb_config_descriptor conf_desc = {
        .bLength = sizeof(conf_desc),
        .bDescriptorType = USB_DT_CONFIG,
        .wTotalLength = 0, // 将在下面设置
        .bNumInterfaces = 1,
        .bConfigurationValue = 1,
        .iConfiguration = 0,
        .bmAttributes = 0x80,
        .bMaxPower = 50  // 100mA
    };

    // 接口描述符 (HID)
    struct usb_interface_descriptor intf_desc = {
        .bLength = sizeof(intf_desc),
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = 0,
        .bAlternateSetting = 0,
        .bNumEndpoints = 2,
        .bInterfaceClass = 0x03,  // HID类
        .bInterfaceSubClass = 0x00,
        .bInterfaceProtocol = 0x00,
        .iInterface = 0
    };

    // HID描述符
    struct usb_hid_descriptor hid_desc = {
        .bLength = sizeof(hid_desc),
        .bDescriptorType = USB_DT_HID,
        .bcdHID = 0x0110,  // HID 1.10
        .bCountryCode = 33,
        .bNumDescriptors = 1,
        .bReportDescriptorType = USB_DT_REPORT,
        .wReportDescriptorLength = 0  // 将在下面设置
    };

    // 端点描述符 (中断输入)
    struct usb_endpoint_descriptor ep_in_desc = {
        .bLength = sizeof(ep_in_desc),
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x81,  // IN端点1
        .bmAttributes = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize = 64,
        .bInterval = 10  // 10ms间隔
    };

    // 端点描述符 (中断输出)
    struct usb_endpoint_descriptor ep_out_desc = {
        .bLength = sizeof(ep_out_desc),
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x02,  // OUT端点2
        .bmAttributes = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize = 64,
        .bInterval = 10  // 10ms间隔
    };

    // 重新计算长度
    hid_desc.wReportDescriptorLength = ups_hid_descriptor_len;  // 设置正确的报告描述符长度
    conf_desc.wTotalLength = sizeof(conf_desc) + sizeof(intf_desc) + sizeof(hid_desc) + sizeof(ep_in_desc) + sizeof(ep_out_desc);

    // 保存设备描述符
    device_descriptor.assign(reinterpret_cast<uint8_t*>(&dev_desc),
                           reinterpret_cast<uint8_t*>(&dev_desc) + sizeof(dev_desc));

    // 保存配置描述符
    config_descriptor.assign(reinterpret_cast<uint8_t*>(&conf_desc),
                           reinterpret_cast<uint8_t*>(&conf_desc) + sizeof(conf_desc));

    // 保存Interface描述符
    interface_descriptor.assign(reinterpret_cast<uint8_t*>(&intf_desc),
                              reinterpret_cast<uint8_t*>(&intf_desc) + sizeof(intf_desc));

    // HID描述符
    hid_descriptor.assign(reinterpret_cast<uint8_t*>(&hid_desc),
                        reinterpret_cast<uint8_t*>(&hid_desc) + sizeof(hid_desc));

    // 保存Endpoint描述符
    endpoint_descriptor.assign(reinterpret_cast<uint8_t*>(&ep_in_desc),
                              reinterpret_cast<uint8_t*>(&ep_in_desc) + sizeof(ep_in_desc));
    endpoint_descriptor.insert(endpoint_descriptor.end(),
                               reinterpret_cast<uint8_t*>(&ep_out_desc),
                               reinterpret_cast<uint8_t*>(&ep_out_desc) + sizeof(ep_out_desc));

    // 保存报告描述符
    report_descriptor.assign(ups_hid_descriptor, ups_hid_descriptor + ups_hid_descriptor_len);

    // 配置描述符拼接其他描述符
    config_descriptor.insert(config_descriptor.end(), interface_descriptor.begin(), interface_descriptor.end());
    config_descriptor.insert(config_descriptor.end(), hid_descriptor.begin(), hid_descriptor.end());
    config_descriptor.insert(config_descriptor.end(),
                             reinterpret_cast<uint8_t*>(&ep_in_desc),
                             reinterpret_cast<uint8_t*>(&ep_in_desc) + sizeof(ep_in_desc));
    config_descriptor.insert(config_descriptor.end(),
                             reinterpret_cast<uint8_t*>(&ep_out_desc),
                             reinterpret_cast<uint8_t*>(&ep_out_desc) + sizeof(ep_out_desc));

}

// 创建字符串描述符的辅助函数
std::vector<uint8_t> UPSDevice::create_string_descriptor(const std::string& str) {
    std::vector<uint8_t> descriptor;

    // 计算描述符长度 (2字节头部 + 每个字符2字节)
    uint8_t length = 2 + str.length() * 2;

    // 添加长度和类型
    descriptor.push_back(length);
    descriptor.push_back(USB_DT_STRING);

    // 添加字符串内容 (每个字符后跟一个0字节，表示UTF-16)
    for (char c : str) {
        descriptor.push_back(static_cast<uint8_t>(c));
        descriptor.push_back(0);
    }

    return descriptor;
}

void UPSDevice::start() {
    if (running) return;

    running = true;
    status_thread = std::thread(&UPSDevice::update_status_loop, this);
}

void UPSDevice::stop() {
    if (!running) return;

    running = false;
    if (status_thread.joinable()) {
        status_thread.join();
    }
}

void UPSDevice::update_status_loop() {
    // Initialize status

    while (running) {
        if (nut_client) {
            auto vars = get_status_from_nut();

            if (vars.size() > 0) {
                std::lock_guard<std::mutex> lock(status_mutex);

                // 更新UPS状态
                for (const auto& pair : vars) {
                    const std::string& var_name = pair.first;
                    const std::string& var_value = pair.second;

                    // 添加调试信息：打印当前处理的变量
                    DEBUG_PRINT("[DEBUG] Processing variable: %s = %s\n", var_name.c_str(), var_value.c_str());

                    try {
                        if (var_name == "input.voltage") {
                            current_status.input_voltage = parse_scaled_u16(var_value, 100.0);
                            DEBUG_PRINT("[DEBUG] Updated input.voltage: %d\n", current_status.input_voltage);
                        } else if (var_name == "output.voltage") {
                            current_status.output_voltage = parse_scaled_u16(var_value, 100.0);
                            DEBUG_PRINT("[DEBUG] Updated output.voltage: %d\n", current_status.output_voltage);
                        } else if (var_name == "battery.voltage") {
                            current_status.battery_voltage = parse_scaled_u16(var_value, 100.0);
                            DEBUG_PRINT("[DEBUG] Updated battery.voltage: %d\n", current_status.battery_voltage);
                        } else if (var_name == "input.current") {
                            current_status.input_current = parse_scaled_u16(var_value, 1000.0);
                            DEBUG_PRINT("[DEBUG] Updated input.current: %d\n", current_status.input_current);
                        } else if (var_name == "output.current") {
                            current_status.output_current = parse_scaled_u16(var_value, 1000.0);
                            DEBUG_PRINT("[DEBUG] Updated output.current: %d\n", current_status.output_current);
                        } else if (var_name == "battery.current") {
                            current_status.battery_current = parse_scaled_u16(var_value, 1000.0);
                            DEBUG_PRINT("[DEBUG] Updated battery.current: %d\n", current_status.battery_current);
                        } else if (var_name == "battery.temperature") {
                            current_status.battery_temperature = parse_scaled_i16(var_value, 100.0);
                            DEBUG_PRINT("[DEBUG] Updated battery.temperature: %d\n", current_status.battery_temperature);
                        } else if (var_name == "battery.capacity") {
                            current_status.battery_capacity = parse_scaled_u16(var_value, 1.0);
                            DEBUG_PRINT("[DEBUG] Updated battery.capacity: %d\n", current_status.battery_capacity);
                        } else if (var_name == "battery.charge") {
                            current_status.battery_charge = parse_scaled_u16(var_value, 1.0);
                            DEBUG_PRINT("[DEBUG] Updated battery.charge: %d\n", current_status.battery_charge);
                        } else if (var_name == "battery.charge.low") {
                            current_status.battery_charge_low = parse_scaled_u16(var_value, 1.0);
                            DEBUG_PRINT("[DEBUG] Updated battery.charge.low: %d\n", current_status.battery_charge_low);
                        } else if (var_name == "battery.charge.warning") {
                            current_status.battery_charge_warning = parse_scaled_u16(var_value, 1.0);
                            DEBUG_PRINT("[DEBUG] Updated battery.charge.warning: %d\n", current_status.battery_charge_warning);
                        } else if (var_name == "ups.load") {
                            current_status.load_percent = parse_scaled_u16(var_value, 1.0);
                            DEBUG_PRINT("[DEBUG] Updated ups.load: %d\n", current_status.load_percent);
                        } else if (var_name == "battery.runtime") {
                            current_status.runtime = parse_scaled_u16(var_value, 1.0);
                            DEBUG_PRINT("[DEBUG] Updated battery.runtime: %d\n", current_status.runtime);
                        } else if (var_name == "battery.runtime.low") {
                            current_status.runtime_low = parse_scaled_u16(var_value, 1.0);
                            DEBUG_PRINT("[DEBUG] Updated battery.runtime.low: %d\n", current_status.runtime_low);
                        } else if (var_name == "input.frequency") {
                            current_status.input_frequency = parse_scaled_u16(var_value, 10.0);
                            DEBUG_PRINT("[DEBUG] Updated input.frequency: %d\n", current_status.input_frequency);
                        } else if (var_name == "ups.status") {
                            // 解析状态字符串
                            DEBUG_PRINT("[DEBUG] Parsing UPS status: %s\n", var_value.c_str());
                            parse_nut_status(var_value);
                        }
                    } catch (...) {
                        DEBUG_PRINT("[DEBUG] Failed to parse %s: %s\n", var_name.c_str(), var_value.c_str());
                    }
                }

                // 打印 current_status.power_summary 的最新结果，逐个属性显示
                DEBUG_PRINT("[DEBUG] After parse_nut_status - power_summary details:\n");
                DEBUG_PRINT("  ac_present: %d\n", (int)current_status.power_summary.ac_present);
                DEBUG_PRINT("  low_battery: %d\n", (int)current_status.power_summary.low_battery);
                DEBUG_PRINT("  low_runtime: %d\n", (int)current_status.power_summary.low_runtime);
                DEBUG_PRINT("  charging: %d\n", (int)current_status.power_summary.charging);
                DEBUG_PRINT("  discharging: %d\n", (int)current_status.power_summary.discharging);
                DEBUG_PRINT("  fully_charged: %d\n", (int)current_status.power_summary.fully_charged);
                DEBUG_PRINT("  overload: %d\n", (int)current_status.power_summary.overload);
                DEBUG_PRINT("  boost: %d\n", (int)current_status.power_summary.boost);
                DEBUG_PRINT("  buck: %d\n", (int)current_status.power_summary.buck);

                // 添加调试信息：打印最终状态更新结果
                DEBUG_PRINT("[DEBUG] Full query completed. New status prepared.\n");
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

std::map<std::string, std::string> UPSDevice::get_status_from_nut() {
    if (!nut_client) {
        return {};
    }

    // 检查是否需要强制全量查询
    auto now = std::chrono::steady_clock::now();
    bool force_full_query = (now - last_full_query_time) >= full_query_interval;

    // 添加调试信息：打印时间间隔检查结果
    DEBUG_PRINT("[DEBUG] Force full query check: %s, Time since last full query: %lld s, Interval: %lld s\n",
                force_full_query ? "true" : "false",
                (long long)std::chrono::duration_cast<std::chrono::seconds>(now - last_full_query_time).count(),
                (long long)std::chrono::duration_cast<std::chrono::seconds>(full_query_interval).count());

    std::string current_status_str = nut_client->get_ups_var("ups.status");

    // 添加调试信息：打印当前状态和上次状态
    DEBUG_PRINT("[DEBUG] Current UPS status: %s, Last status: %s\n",
                current_status_str.c_str(), last_status.c_str());

    // 如果状态发生变化或需要强制全量查询，则获取所有变量
    if (force_full_query || current_status_str != last_status) {
        DEBUG_PRINT("[DEBUG] Performing full query\n");

        last_status = current_status_str;
        last_full_query_time = now;

        // 获取所有UPS变量
        auto vars = nut_client->get_ups_vars();

        if (vars.find("ups.status") == vars.end()) {
            vars["ups.status"] = current_status_str;
        }

        // 添加调试信息：打印获取到的变量数量
        DEBUG_PRINT("[DEBUG] Retrieved %zu UPS variables\n", vars.size());

        return vars;

    }
    // 电池模式, 同步更新电量和剩余时间
    else if (current_status_str.find("OB") != std::string::npos) {
        std::string charge = nut_client->get_ups_var("battery.charge");
        std::string runtime = nut_client->get_ups_var("battery.runtime");

        return {
            {"battery.charge", charge},
            {"battery.runtime", runtime}
        };
    } else {
        DEBUG_PRINT("[DEBUG] Status unchanged, no action needed\n");
        return {};
    }
}

void UPSDevice::parse_nut_status(const std::string& nut_status) {
    // NUT状态字符串通常包含多个标志，如 "OL CHRG" 表示在线充电
    // 解析状态字符串并设置相应的标志位
    current_status.power_summary.ac_present = 0;
    current_status.power_summary.charging = 0;
    current_status.power_summary.discharging = 0;
    current_status.power_summary.fully_charged = 0;
    current_status.power_summary.low_runtime = 0;
    current_status.power_summary.low_battery = 0;
    current_status.power_summary.overload = 0;
    current_status.power_summary.boost = 0;
    current_status.power_summary.buck = 0;

    std::string status_str = " " + nut_status + " ";

    // 检查是否包含关键状态标志
    if (status_str.find(" OL ") != std::string::npos) {
        // On Line - 市电正常
        current_status.power_summary.ac_present = UPS_AC_PRESENT;
    } else if (status_str.find(" OB ") != std::string::npos) {
        // On Battery - 电池供电
        current_status.power_summary.discharging = UPS_DISCHARGING;
    }

    if (status_str.find(" CHRG ") != std::string::npos) {
        // Charging - 正在充电
        current_status.power_summary.charging = UPS_CHARGING;
    }

    if (status_str.find(" DISCHRG ") != std::string::npos) {
        // Discharging - 正在放电
        current_status.power_summary.discharging = UPS_DISCHARGING;
    }

    if (status_str.find(" LB ") != std::string::npos) {
        // Low Battery - 电量低
        current_status.power_summary.low_battery = UPS_LOW_BATTERY;
    }

    if (status_str.find(" OVER ") != std::string::npos) {
        // Overload - 过载
        current_status.power_summary.overload = UPS_OVERLOAD;
    }

    if (status_str.find(" BOOST ") != std::string::npos) {
        // Boost - 升压
        current_status.power_summary.boost = UPS_BOOST;
    }

    if (status_str.find(" BUCK ") != std::string::npos) {
        // Buck - 降压
        current_status.power_summary.buck = UPS_BUCK;
    }

    if (status_str.find(" FSD ") != std::string::npos) {
        // Shutdown - 关机
        current_status.power_summary.low_runtime = UPS_LOW_RUNTIME;
    }
}

ups_status UPSDevice::get_status() {
    std::lock_guard<std::mutex> lock(status_mutex);
    return current_status;
}

const std::vector<uint8_t>& UPSDevice::get_string_descriptor(int index) const {
    if (index >= 0 && index < MAX_STRING_COUNT && !string_descriptors[index].empty()) {
        return string_descriptors[index];
    }
    static std::vector<uint8_t> empty;
    return empty;
}

int UPSDevice::handle_control_request(const struct usb_setup_packet* setup_packet, uint8_t* response_data) {
    // 使用setup包结构体
    uint16_t wValue = setup_packet->wValue;
    uint16_t wIndex = setup_packet->wIndex;
    uint16_t wLength = setup_packet->wLength;
    uint8_t ResourceType = wValue >> 8;
    uint8_t ResourceId = wValue & 0xFF;

    int response_length = 0;

    switch (setup_packet->bRequest) {
        case USB_REQ_GET_DESCRIPTOR:
            switch (ResourceType) {  // Descriptor Type
                case USB_DT_DEVICE:
                    DEBUG_PRINT("GET_DESCRIPTOR: DEVICE\n");
                    response_length = std::min((int)device_descriptor.size(), (int)wLength);
                    memcpy(response_data, device_descriptor.data(), response_length);
                    DEBUG_PRINT("Device descriptor response length: %d\n", response_length);
                    break;

                case USB_DT_CONFIG:
                    DEBUG_PRINT("GET_DESCRIPTOR: CONFIG\n");
                    response_length = std::min((int)config_descriptor.size(), (int)wLength);
                    memcpy(response_data, config_descriptor.data(), response_length);
                    DEBUG_PRINT("Config descriptor response length: %d\n", response_length);
                    break;

                case USB_DT_STRING:
                    DEBUG_PRINT("GET_DESCRIPTOR: STRING %d\n", ResourceId);
                    {
                        int string_index = ResourceId;
                        const auto& str_desc = get_string_descriptor(string_index);
                        response_length = std::min((int)str_desc.size(), (int)wLength);
                        memcpy(response_data, str_desc.data(), response_length);
                        DEBUG_PRINT("String descriptor response length: %d\n", response_length);
                    }
                    break;

                case USB_DT_INTERFACE:
                    DEBUG_PRINT("GET_DESCRIPTOR: INTERFACE\n");
                    // 直接返回预构建的接口描述符
                    if (wLength >= sizeof(usb_interface_descriptor)) {
                        response_length = std::min((int)interface_descriptor.size(), (int)wLength);
                        memcpy(response_data, interface_descriptor.data(), response_length);
                        DEBUG_PRINT("Interface descriptor response length: %d\n", response_length);
                    } else {
                        response_length = 0;
                    }
                    break;

                case USB_DT_ENDPOINT:
                    DEBUG_PRINT("GET_DESCRIPTOR: ENDPOINT\n");
                    if (wLength >= sizeof(usb_endpoint_descriptor)) {
                        size_t offset = 0;
                        if (ResourceId == 0x02 || ResourceId == 2 || wIndex == 2) {
                            offset = sizeof(usb_endpoint_descriptor);
                        } else if (!(ResourceId == 0x81 || ResourceId == 1 || wIndex == 0 || wIndex == 1)) {
                            response_length = 0;
                            break;
                        }

                        if (endpoint_descriptor.size() >= offset + sizeof(usb_endpoint_descriptor)) {
                            response_length = std::min((int)sizeof(usb_endpoint_descriptor), (int)wLength);
                            memcpy(response_data, endpoint_descriptor.data() + offset, response_length);
                            DEBUG_PRINT("Endpoint descriptor response length: %d\n", response_length);
                        }
                    }
                    break;

                case USB_DT_HID:
                    DEBUG_PRINT("GET_DESCRIPTOR: HID\n");
                    // 直接使用预设的HID描述符变量
                    response_length = std::min((int)hid_descriptor.size(), (int)wLength);
                    memcpy(response_data, hid_descriptor.data(), response_length);
                    DEBUG_PRINT("HID descriptor response length: %d\n", response_length);
                    break;

                case USB_DT_REPORT:
                    DEBUG_PRINT("GET_DESCRIPTOR: REPORT\n");
                    response_length = std::min((int)report_descriptor.size(), (int)wLength);
                    memcpy(response_data, report_descriptor.data(), response_length);
                    DEBUG_PRINT("Report descriptor response length: %d\n", response_length);
                    break;

                default:
                    DEBUG_PRINT("Unknown descriptor type: 0x%x\n", ResourceType);
                    // 对于未知描述符类型，返回空数据而不是错误
                    response_length = 0;
                    break;
            }
            break;

        case USB_REQ_GET_STATUS:
            DEBUG_PRINT("GET_STATUS\n");
            if (wLength >= 2) {
                response_data[0] = 0x01;  // 自供电，不支持远程唤醒
                response_data[1] = 0x00;
                response_length = 2;
            }
            break;

        case USB_REQ_SET_ADDRESS:
            DEBUG_PRINT("SET_ADDRESS: %d\n", wValue);
            // 设置地址，不需要返回数据
            response_length = 0;
            break;

        case USB_REQ_SET_CONFIGURATION:
            DEBUG_PRINT("SET_CONFIGURATION: %d\n", wValue);
            // 设置配置，不需要返回数据
            response_length = 0;
            break;

        case USB_REQ_GET_CONFIGURATION:
            DEBUG_PRINT("GET_CONFIGURATION\n");
            if (wLength >= 1) {
                response_data[0] = 1;  // 配置值
                response_length = 1;
            }
            break;

        case USB_REQ_SET_INTERFACE:
            DEBUG_PRINT("SET_INTERFACE: %d, %d\n", wValue, wIndex);
            // 设置接口，不需要返回数据
            response_length = 0;
            break;

        case USB_REQ_GET_INTERFACE:
            DEBUG_PRINT("GET_INTERFACE\n");
            if (wLength >= 1) {
                response_data[0] = 0;  // 替代设置
                response_length = 1;
            }
            break;

        case HID_REQ_GET_REPORT:
            DEBUG_PRINT("HID_GET_REPORT\n");
            response_length = get_report(ResourceType, ResourceId, response_data, setup_packet->wLength);
            break;

        case HID_REQ_GET_IDLE:
            DEBUG_PRINT("HID_GET_IDLE\n");
            if (wLength >= 1) {
                response_data[0] = 0;  // 空闲速率
                response_length = 1;
            }
            break;

        case HID_REQ_GET_PROTOCOL:
            DEBUG_PRINT("HID_GET_PROTOCOL\n");
            if (wLength >= 1) {
                response_data[0] = 1;  // 报告协议
                response_length = 1;
            }
            break;

        default:
            DEBUG_PRINT("Unknown control request: 0x%x\n", (int)(setup_packet->bRequest));
            // 对于未知请求，返回空数据而不是错误
            response_length = 0;
            break;
    }

    DEBUG_PRINT("Final response length: %d\n", response_length);
    return response_length;
}

int UPSDevice::handle_interrupt_request(const struct usb_setup_packet* setup_packet, uint8_t* response_data) {
    // 避免未使用参数警告
    (void)setup_packet;
    (void)response_data;

    // 固定等待500毫秒, 不会导致主机触发 USB 的 UNLINK 事件
    uint32_t wait_time = 500;
    DEBUG_PRINT("Interrupt IN: waiting %d ms before NAK\n", wait_time);

    // 等待主机查询间隔
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_time));

    // 返回 0 表示 NAK，无数据返回
    return 0;
}

int UPSDevice::get_report(uint8_t report_type, uint8_t report_id, uint8_t* response_data, uint16_t response_length) {
    DEBUG_PRINT("GET_REPORT: type=%d, id=%d, length=%d\n", (int)report_type, (int)report_id, response_length);

    if (report_type == 0x02) {
        DEBUG_PRINT("Output report requested\n");
        return 0;
    }

    if (report_type != 0x01 && report_type != 0x03) {
        DEBUG_PRINT("Unknown report type: %d\n", (int)report_type);
        return 0;
    }

    auto append_u16 = [](std::vector<uint8_t>& report, uint16_t value) {
        report.push_back(static_cast<uint8_t>(value & 0xff));
        report.push_back(static_cast<uint8_t>(value >> 8));
    };

    auto copy_report = [&](const std::vector<uint8_t>& report) -> int {
        int actual_length = std::min<int>(static_cast<int>(report.size()), response_length);
        if (actual_length > 0) {
            memcpy(response_data, report.data(), actual_length);
        }
        return actual_length;
    };

    switch (report_id) {
        case 0x01:
            // Static PowerSummary fields: iManufacturer/iProduct/iSerialNumber,
            // iDeviceChemistry, iOEMInformation and CapacityMode.
            return copy_report({
                0x01,
                0x00,
                0x01, // iManufacturer -> string descriptor 1
                0x02, // iProduct -> string descriptor 2
                0x03, // iSerialNumber -> string descriptor 3
                0x05, // iDeviceChemistry -> string descriptor 5
                0x04, // iOEMInformation -> string descriptor 4
                0x02  // CapacityMode: percent
            });

        case 0x02: {
            ups_status status = get_status();
            uint16_t status_bits = 0;
            bool low_charge = status.power_summary.low_battery ||
                              status.battery_charge <= status.battery_charge_low;
            bool low_runtime = status.power_summary.low_runtime ||
                               status.runtime <= status.runtime_low;

            status_bits |= (1 << 0); // Good
            if (status.power_summary.overload) {
                status_bits |= (1 << 2); // Overload
            }
            if (status.power_summary.low_runtime) {
                status_bits |= (1 << 4); // ShutdownImminent
            }
            if (low_charge) {
                status_bits |= (1 << 5); // BelowRemainingCapacityLimit
            }
            if (low_runtime) {
                status_bits |= (1 << 6); // RemainingTimeLimitExpired
            }
            if (status.power_summary.charging) {
                status_bits |= (1 << 7); // Charging
            }
            if (status.power_summary.discharging) {
                status_bits |= (1 << 8); // Discharging
            }
            if (status.power_summary.ac_present) {
                status_bits |= (1 << 10); // ACPresent
            }
            status_bits |= (1 << 11); // BatteryPresent

            std::vector<uint8_t> report;
            report.reserve(23);
            report.push_back(0x02);
            append_u16(report, status.battery_voltage);
            append_u16(report, status.battery_current);
            report.push_back(percent_to_u8(status.battery_capacity));
            report.push_back(percent_to_u8(status.battery_charge));
            report.push_back(percent_to_u8(status.battery_charge_low));
            report.push_back(percent_to_u8(status.battery_charge_warning));
            append_u16(report, status.runtime);
            append_u16(report, openups_temperature_raw(status.battery_temperature));
            append_u16(report, status.output_voltage);
            append_u16(report, status.output_current);
            append_u16(report, status.input_voltage);
            append_u16(report, status.input_current);
            append_u16(report, status_bits);
            return copy_report(report);
        }

        case 0x03: {
            ups_status status = get_status();
            return copy_report({
                0x03,
                percent_to_u8(status.battery_capacity)
            });
        }

        default:
            DEBUG_PRINT("Unknown openUPS report ID: %d\n", (int)report_id);
            return 0;
    }
}
