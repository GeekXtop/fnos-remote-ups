#ifndef USBIP_SERVER_H
#define USBIP_SERVER_H

#include <atomic>
#include <string>
#include <memory>
#include <map>
#include <uv.h>

#include "usb_structures.h"
#include "ups_device.h"

// 前向声明
class USBIPServer;

// 客户端连接状态
enum class ClientState {
    WAITING_COMMAND,      // 等待命令
    WAITING_IMPORT_BUSID, // 等待导入 busid
    IMPORTED,             // 已导入，处理 USB 命令
    DEVLIST_REQUESTED     // 设备列表请求中
};

// 客户端连接上下文
struct ClientContext {
    uv_tcp_t handle;
    USBIPServer* server;
    ClientState state;
    std::string busid;
    std::string client_ip;  // 客户端 IP 地址
    std::vector<char> recv_buffer;  // 接收数据缓存
    int pending_seqnum;

    // 中断请求定时器映射：seqnum -> timer
    std::map<uint32_t, uv_timer_t*> interrupt_timers;

    ClientContext() : server(nullptr), state(ClientState::WAITING_COMMAND), pending_seqnum(0) {}
};

// 定时器数据结构
struct TimerData {
    ClientContext* ctx;
    uint32_t seqnum;
};

struct ResponseData {
    const char* buf;
    size_t len;
};

// USB/IP 服务器类
class USBIPServer {
private:
    uv_loop_t* loop;  // 外部传入的事件循环
    uv_tcp_t server_handle;
    struct sockaddr_in bind_addr;
    std::atomic<bool> running;
    UPSDevice ups_device;
    std::string ups_identifier;

    // 客户端连接管理（on_client_close 是唯一的释放点，不用 shared_ptr 避免 double-free）
    std::map<uv_tcp_t*, ClientContext*> clients;

    // 静态回调函数 (C 风格，用于 libuv)
    static void on_connection(uv_stream_t* server, int status);
    static void on_client_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf);
    static void on_client_write(uv_write_t* req, int status);
    static void on_client_close(uv_handle_t* handle);
    static void on_alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);

    // 实例方法处理具体逻辑
    ssize_t handle_client_data(ClientContext* ctx, const char* data, ssize_t nread);
    int handle_devlist_request(ClientContext* ctx);
    int handle_import_request(ClientContext* ctx, const char* busid);
    int handle_usb_command(ClientContext* ctx, const union usbip_header* header);

    // 定时器处理
    static void on_interrupt_timer(uv_timer_t* timer);
    void cancel_interrupt_timer(ClientContext* ctx, uint32_t seqnum);
    void cancel_all_interrupt_timers(ClientContext* ctx);

    // 发送响应
    void send_response(ClientContext* ctx, const ResponseData* buffers, size_t count);

    // 客户端管理
    void add_client(ClientContext* ctx);
    void remove_client(ClientContext* ctx);

public:
    // 必须传入外部事件循环
    USBIPServer(uv_loop_t* loop,
                const std::string& ups_identifier,
                const std::string& manufacturer = DEFAULT_DEVICE_MANUFACTURER,
                const std::string& product = DEFAULT_DEVICE_PRODUCT,
                uint16_t vendor_id = DEFAULT_DEVICE_VENDOR_ID,
                uint16_t product_id = DEFAULT_DEVICE_PRODUCT_ID);
    ~USBIPServer();

    // 启动服务器（不再阻塞运行事件循环）
    bool start(int port = USBIP_PORT);
    void stop();
    bool isRunning() const { return running.load(); }
    uv_loop_t* getLoop() const { return loop; }
};

#endif // USBIP_SERVER_H
