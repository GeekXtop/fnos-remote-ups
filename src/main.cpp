#include <iostream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <memory>
#include <atomic>
#include <csignal>
#include <uv.h>

#include "usbip_server.h"
#include "device_monitor.h"

// 全局调试标志
bool g_debug = false;

// 全局事件循环
static uv_loop_t* g_loop = nullptr;

// 全局服务器指针，用于信号处理
static USBIPServer* g_server = nullptr;

// 异步信号处理
static uv_async_t g_signal_async;
static std::atomic<int> g_signal_received(0);

// 信号处理回调（在 libuv 事件循环中执行）
void on_signal_async(uv_async_t* handle) {
    (void)handle;  // 避免未使用参数警告
    int sig = g_signal_received.load();
    if (sig != 0) {
        fprintf(stderr, "\n[Signal] Received signal %d (%s), shutting down gracefully...\n",
                sig, strsignal(sig));
        if (g_server) {
            g_server->stop();
        }
        // 停止事件循环
        uv_stop(g_loop);
    }
}

// 系统信号处理函数（在信号上下文中执行，只能做最小工作）
void signal_handler(int signum) {
    g_signal_received.store(signum);
    uv_async_send(&g_signal_async);
}

int main(int argc, char* argv[]) {
    int port = USBIP_PORT;
    std::string ups_identifier;
    std::string auto_mount_host;
    int auto_mount_port = 0;
    std::string auto_mount_bus_id;
    bool auto_mount_enabled = false;
    std::string manufacturer = DEFAULT_DEVICE_MANUFACTURER;
    std::string product = DEFAULT_DEVICE_PRODUCT;

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            ups_identifier = argv[i + 1];
            i++;
        } else if ((strcmp(argv[i], "-M") == 0 || strcmp(argv[i], "--manufacturer") == 0) && i + 1 < argc) {
            manufacturer = argv[i + 1];
            i++;
        } else if ((strcmp(argv[i], "-P") == 0 || strcmp(argv[i], "--product") == 0) && i + 1 < argc) {
            product = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "-m") == 0) {
            // 解析 -m <host>:<port>@<bus-id> 参数
            std::string mount_arg;
            if (i+1 < argc && argv[i+1][0] != '-') {
                mount_arg = argv[++i];
            }
            if (!mount_arg.empty()) {
                // 解析 @ 分隔符，格式：<host>:<port>@<bus-id>
                size_t at_pos = mount_arg.find('@');
                if (at_pos != std::string::npos) {
                    // 有 @，解析 host:port 部分
                    std::string host_port = mount_arg.substr(0, at_pos);
                    auto_mount_bus_id = mount_arg.substr(at_pos + 1);

                    // 解析 host:port 部分
                    size_t colon_pos = host_port.find(':');
                    if (colon_pos != std::string::npos) {
                        auto_mount_host = host_port.substr(0, colon_pos);
                        auto_mount_port = std::stoi(host_port.substr(colon_pos + 1));
                    } else {
                        auto_mount_host = host_port;
                    }
                } else {
                    // 没有 @，只有 host 或 host:port
                    size_t colon_pos = mount_arg.find(':');
                    if (colon_pos != std::string::npos) {
                        auto_mount_host = mount_arg.substr(0, colon_pos);
                        auto_mount_port = std::stoi(mount_arg.substr(colon_pos + 1));
                    } else {
                        auto_mount_host = mount_arg;
                    }
                    auto_mount_bus_id = "1-1";  // 默认 bus-id
                }
            } else {
                auto_mount_host = "127.0.0.1";
                auto_mount_bus_id = "1-1";
            }
            auto_mount_enabled = true;
        } else if (strcmp(argv[i], "-d") == 0) {
            g_debug = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            std::cout << "USB/IP UPS Server (libuv)" << std::endl;
            std::cout << "Usage: " << argv[0] << " [-p port] [-u ups_name@server-ip-or-domain[:port]] [-M manufacturer] [-P product] [-m [host:port@bus-id]] [-d] [-h]" << std::endl;
            std::cout << "  -p port    Set listening port (default: 3240)" << std::endl;
            std::cout << "  -u ups     Set remote UPS identifier (required format: ups_name@server-ip-or-domain[:port])" << std::endl;
            std::cout << "  -M name    Set USB manufacturer string (default: " << DEFAULT_DEVICE_MANUFACTURER << ")" << std::endl;
            std::cout << "  -P name    Set USB product string (default: " << DEFAULT_DEVICE_PRODUCT << ")" << std::endl;
            std::cout << "  -m mount   Enable auto-mount for USB device (format: [host:port@bus-id], default host: 127.0.0.1, port: 3240, bus-id: 1-1)" << std::endl;
            std::cout << "  -d         Enable debug output" << std::endl;
            std::cout << "  -h         Show this help" << std::endl;
            return 0;
        }
    }

    if (ups_identifier.empty()) {
        std::cerr << "Error: UPS identifier is required. Use -u ups_name@server-ip-or-domain[:port]" << std::endl;
        std::cout << "Usage: " << argv[0] << " [-p port] [-u ups_name@server-ip-or-domain[:port]] [-M manufacturer] [-P product] [-m [host:port@bus-id]] [-d] [-h]" << std::endl;
        return 1;
    }

    if (auto_mount_port == 0) {
        auto_mount_port = port;
    }

    // 初始化 libuv 事件循环
    g_loop = uv_loop_new();
    if (!g_loop) {
        std::cerr << "Failed to create libuv loop" << std::endl;
        return 1;
    }

    // 初始化异步处理句柄
    uv_async_init(g_loop, &g_signal_async, on_signal_async);

    // 注册信号处理函数，监听 Docker 停止容器的默认信号 (SIGTERM, SIGINT)
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    printf("USB/IP UPS Server for NUT (Network UPS Tools)\n");
    printf("Project URL: https://github.com/iwinmin/fnos-remote-ups\n");
    printf("Developer: Winmin\n");
    printf("Starting USB/IP UPS Server for remote UPS: %s...\n", ups_identifier.c_str());
    printf("USB identity: %s / %s\n", manufacturer.c_str(), product.c_str());

    // 创建并启动服务器（传入全局事件循环）
    USBIPServer server(g_loop, ups_identifier, manufacturer, product);
    g_server = &server;  // 设置全局指针

    // 如果启用了自动挂载，启动监控线程
    std::unique_ptr<DeviceMonitor> monitor(new DeviceMonitor(
        auto_mount_host,
        auto_mount_port,
        auto_mount_bus_id,
        DEVICE_VENDOR_ID,
        DEVICE_PRODUCT_ID,
        DEVICE_SERIAL_NUMBER
    ));
    if (auto_mount_enabled && !auto_mount_host.empty() && !auto_mount_bus_id.empty()) {
        fprintf(stderr, "[AutoMount] Starting device monitor for %s:%d@%s\n", auto_mount_host.c_str(), auto_mount_port, auto_mount_bus_id.c_str());
        monitor->start();
    }

    // 启动服务器（非阻塞，只注册监听）
    if (!server.start(port)) {
        std::cerr << "Failed to start server" << std::endl;
        if (monitor) {
            monitor->stop();
        }
        uv_loop_delete(g_loop);
        g_loop = nullptr;
        return 1;
    }

    // 运行事件循环（阻塞直到 uv_stop 被调用）
    uv_run(g_loop, UV_RUN_DEFAULT);

    // 服务器已停止，停止监控线程
    if (monitor) {
        monitor->stop();
    }

    // 清理 libuv
    uv_close((uv_handle_t*)&g_signal_async, nullptr);
    uv_run(g_loop, UV_RUN_DEFAULT);
    uv_loop_delete(g_loop);
    g_loop = nullptr;

    fprintf(stderr, "[Shutdown] Program exited gracefully\n");
    return 0;
}
