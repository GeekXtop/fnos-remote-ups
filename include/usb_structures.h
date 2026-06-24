#ifndef USB_STRUCTURES_H
#define USB_STRUCTURES_H

#include <cstdint>
#include <vector>
#include <map>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>

#define DEVICE_VENDOR_ID 0x0764
#define DEVICE_PRODUCT_ID 0x0501
#define DEFAULT_DEVICE_MANUFACTURER "WalleCube"
#define DEFAULT_DEVICE_PRODUCT "Smart UPS W150"
#define DEVICE_OEM_INFO "WinminVirtualUPS"
#define DEVICE_SERIAL_NUMBER "VPS-" SERIAL_DATE
#define DEVICE_BATTERY_TYPE "PbAcid"

// USB/IP协议常量
#define USBIP_VERSION 0x0111
#define USBIP_PORT 3240
#define OP_REQ_DEVLIST 0x8005
#define OP_REQ_IMPORT  0x8003
#define OP_REP_DEVLIST 0x0005
#define OP_REP_IMPORT  0x0003
#define USBIP_CMD_SUBMIT 0x0001
#define USBIP_CMD_UNLINK 0x0002
#define USBIP_RET_SUBMIT 0x0003
#define USBIP_RET_UNLINK 0x0004

// USB常量
#define USB_DIR_OUT 0x00
#define USB_DIR_IN  0x80
#define USB_TYPE_MASK 0x60
#define USB_TYPE_STANDARD 0x00
#define USB_TYPE_CLASS 0x20
#define USB_TYPE_VENDOR 0x40
#define USB_TYPE_RESERVED 0x60
#define USB_RECIP_MASK 0x1f
#define USB_RECIP_DEVICE 0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT 0x02
#define USB_RECIP_OTHER 0x03

// USB速度常量
#define USB_SPEED_UNKNOWN 0
#define USB_SPEED_LOW 1
#define USB_SPEED_FULL 2
#define USB_SPEED_HIGH 3
#define USB_SPEED_WIRELESS 4
#define USB_SPEED_SUPER 5
#define USB_SPEED_SUPER_PLUS 6

// USB标准请求
#define USB_REQ_GET_STATUS 0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE 0x03
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_DESCRIPTOR 0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE 0x0A
#define USB_REQ_SET_INTERFACE 0x0B
#define USB_REQ_SYNCH_FRAME 0x12

// USB描述符类型
#define USB_DT_DEVICE 0x01
#define USB_DT_CONFIG 0x02
#define USB_DT_STRING 0x03
#define USB_DT_INTERFACE 0x04
#define USB_DT_ENDPOINT 0x05
#define USB_DT_DEVICE_QUALIFIER 0x06
#define USB_DT_OTHER_SPEED_CONFIG 0x07
#define USB_DT_INTERFACE_POWER 0x08
#define USB_DT_HID 0x21
#define USB_DT_REPORT 0x22

// HID类请求
#define HID_REQ_GET_REPORT 0x01
#define HID_REQ_GET_IDLE 0x02
#define HID_REQ_GET_PROTOCOL 0x03
#define HID_REQ_SET_REPORT 0x09
#define HID_REQ_SET_IDLE 0x0A
#define HID_REQ_SET_PROTOCOL 0x0B

// USB端点类型
#define USB_ENDPOINT_XFER_CONTROL 0
#define USB_ENDPOINT_XFER_ISOC 1
#define USB_ENDPOINT_XFER_BULK 2
#define USB_ENDPOINT_XFER_INT 3

// 状态位
#define UPS_AC_PRESENT (1 << 0) // AC Present
#define UPS_CHARGING (1 << 1) // Charging
#define UPS_DISCHARGING (1 << 2) // Discharging
#define UPS_FULLY_CHARGED (1 << 3) // Fully Charged
#define UPS_LOW_RUNTIME (1 << 4) // Low Runtime
#define UPS_LOW_BATTERY (1 << 5) // Low Battery

#define UPS_OVERLOAD (1 << 0) // Overload
#define UPS_BOOST (1 << 1) // Boost
#define UPS_BUCK (1 << 2) // Buck

#define MAX_STRING_COUNT 10

// USBIP命令头部结构
struct usbip_message_header {
    uint16_t version;
    uint16_t command;
    uint32_t status;
} __attribute__((packed));

// USBIP头部结构
struct usbip_header_basic {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
};

struct usbip_header_cmd_submit {
    uint32_t transfer_flags;
    uint32_t transfer_buffer_length;
    uint32_t start_frame;
    uint32_t number_of_packets;
    uint32_t interval;
    uint8_t  setup[8];
};

struct usbip_header_ret_submit {
    uint32_t status;
    uint32_t actual_length;
    uint32_t start_frame;
    uint32_t number_of_packets;
    uint32_t error_count;
    uint8_t  padding[8];
};

struct usbip_header_cmd_unlink {
    uint32_t seqnum;
    uint8_t  padding[24];
};

struct usbip_header_ret_unlink {
    uint32_t status;
    uint8_t  padding[24];
};

union usbip_header {
    struct usbip_header_basic base;
    struct {
        struct usbip_header_basic base;
        struct usbip_header_cmd_submit cmd_submit;
    } cmd_submit;
    struct {
        struct usbip_header_basic base;
        struct usbip_header_ret_submit ret_submit;
    } ret_submit;
    struct {
        struct usbip_header_basic base;
        struct usbip_header_cmd_unlink data;
    } cmd_unlink;
    struct {
        struct usbip_header_basic base;
        struct usbip_header_ret_unlink data;
    } ret_unlink;
};

// USB Setup数据包结构体
struct usb_setup_packet {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

// USB设备描述符
struct usb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed));

// USB配置描述符
struct usb_config_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed));

// USB接口描述符
struct usb_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed));

// USB端点描述符
struct usb_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed));

// HID描述符
struct usb_hid_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bReportDescriptorType;
    uint16_t wReportDescriptorLength;
} __attribute__((packed));

// USB/IP设备信息
struct usbip_device {
    char path[256];
    char busid[32];
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bConfigurationValue;
    uint8_t bNumConfigurations;
    uint8_t bNumInterfaces;
} __attribute__((packed));

// USB/IP接口信息
struct usbip_interface {
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t padding;  // 对齐
} __attribute__((packed));

// UPS状态数据
struct ups_status {
    uint16_t input_voltage;    // 输入电压 (V)
    uint16_t output_voltage;   // 输出电压 (V)
    uint16_t battery_voltage;  // 电池电压 (V)
    uint16_t battery_charge;   // 电池电量 (%)
    uint16_t load_percent;     // 负载百分比 (%)
    uint16_t runtime;          // 剩余运行时间 (秒)
    uint16_t runtime_low;      // 警告剩余时间
    uint16_t input_frequency;  // 市电频率 (Hz)

    struct {
        uint8_t ac_present;
        uint8_t low_battery;
        uint8_t low_runtime;
        uint8_t charging;
        uint8_t discharging;
        uint8_t fully_charged;
        uint8_t overload;
        uint8_t boost;
        uint8_t buck;
    } power_summary;
};

// 全局设备信息
extern struct usbip_device global_devinfo;

// 辅助函数声明
void write_uint16_le(uint8_t* buf, uint16_t val);

#endif // USB_STRUCTURES_H
