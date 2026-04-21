#include "nut_client.h"
#include "usb_structures.h"
#include <cstring>

// 引入调试宏
extern bool g_debug;
#define DEBUG_PRINT(fmt, ...) \
    do { if (g_debug) fprintf(stderr, fmt, ##__VA_ARGS__); } while(0)

NUTClient::NUTClient(const std::string& ups_identifier) : socket_fd(-1), connected(false) {
    // 解析 ups_identifier 格式: ups_name@server-ip-or-domain:port
    size_t at_pos = ups_identifier.find('@');
    if (at_pos != std::string::npos) {
        ups_name = ups_identifier.substr(0, at_pos);
        std::string server_info = ups_identifier.substr(at_pos + 1);

        size_t colon_pos = server_info.find_last_of(':');
        if (colon_pos != std::string::npos) {
            server_host = server_info.substr(0, colon_pos);
            server_port = std::stoi(server_info.substr(colon_pos + 1));
        } else {
            server_host = server_info;
            server_port = 3493; // 默认NUT端口
        }
    } else {
        throw std::invalid_argument("Invalid UPS identifier format. Expected: ups_name@server-ip-or-domain[:port]");
    }
}

NUTClient::~NUTClient() {
    disconnect();
}

bool NUTClient::connect() {
    // 创建socket
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        DEBUG_PRINT("Failed to create socket\n");
        return false;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    // 尝试将主机名解析为IP地址
    if (inet_pton(AF_INET, server_host.c_str(), &server_addr.sin_addr) <= 0) {
        // 如果不是有效的IP地址，尝试DNS解析
        struct hostent* host_entry = gethostbyname(server_host.c_str());
        if (host_entry == nullptr) {
            DEBUG_PRINT("Failed to resolve host: %s\n", server_host.c_str());
            close(socket_fd);
            socket_fd = -1;
            return false;
        }
        memcpy(&server_addr.sin_addr, host_entry->h_addr_list[0], host_entry->h_length);
    }

    // 连接到NUT服务器
    if (::connect(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        DEBUG_PRINT("Failed to connect to NUT server: %s:%d\n", server_host.c_str(), server_port);
        close(socket_fd);
        socket_fd = -1;
        return false;
    }

    connected = true;
    return true;
}

void NUTClient::disconnect() {
    if (socket_fd >= 0) {
        close(socket_fd);
        socket_fd = -1;
    }
    connected = false;
}

// 发送命令并接收响应
std::string NUTClient::send_command(const std::string& command) {
    if (!connected && !connect()) {
        return "ERR CONNECT_FAILED";
    }

    // 发送命令
    std::string cmd_with_newline = command + "\n";
    DEBUG_PRINT("[NUT] Sending command: %s", cmd_with_newline.c_str());
    if (::send(socket_fd, cmd_with_newline.c_str(), cmd_with_newline.length(), 0) < 0) {
        DEBUG_PRINT("Failed to send command to NUT server\n");
        disconnect();
        return "ERR SEND_CMD_FAILED";
    }

    // 接收响应
    std::string response;
    char buffer[8192];
    ssize_t recv_size;
    size_t total_received = 0;
    size_t line_size = 0;
    char* recv_buf = buffer;

    // 先循环接收, 直到接收到首行的换行符 "\n"
    while (total_received < sizeof(buffer)) {
        recv_size = recv(socket_fd, recv_buf, sizeof(buffer) - total_received, 0);
        if (recv_size <= 0) {
            DEBUG_PRINT("Failed to receive response from NUT server\n");
            disconnect();
            return "ERR RECEIVE_FAILED";
        }
        total_received += recv_size;
        recv_buf += recv_size;

        for (ssize_t i = 0; i < recv_size; i++) {
            if (buffer[line_size] == '\n') {
                goto got_line;
            }
            line_size++;
        }
    }

    // 未找到首行字符串
    DEBUG_PRINT("Failed to receive response from NUT server\n");
    disconnect();
    return "ERR RECEIVE_FAILED";

    got_line:
    std::string line = std::string(buffer, line_size);
    line_size++;

    if (strncmp(buffer, "BEGIN ", 6) != 0) {
        // 非分块响应, 移除行末尾的换行符后返回
        if (line_size == total_received) {
            return line;
        }
        else {
            // 非单行响应, 返回错误
            return "ERR MULTI_LINE_RESPONSE";
        }
    }

    // 处理分块响应
    response = std::string(buffer + line_size, total_received - line_size);

    // 提取首行, 把行首的 BEGIN 换成 END 作为结束行
    std::string first_line = std::string(buffer, line_size-1); // 提取首行 (去掉换行符)

    // 从首行构造结束行，将 BEGIN 替换为 END
    line.replace(0, 5, "\nEND");

    // 继续接收剩余部分, 直到接收到结束行
    while (total_received < 65535) {
        // 检查当前已接收的数据中是否包含结束行
        size_t end_pos = response.find(line);
        if (end_pos != std::string::npos) {
            // 找到结束行，提取 BEGIN 和 END 之间的内容
            response = response.substr(0, end_pos);
            return response;
        }

        // 继续接收更多数据
        recv_size = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (recv_size <= 0) {
            DEBUG_PRINT("Failed to receive response from NUT server\n");
            disconnect();
            return "ERR RECEIVE_FAILED";
        }
        response += std::string(buffer, recv_size);

        total_received += recv_size;
    }

    return "ERR RECEIVE_TOO_LONG_RESPONSE";
}

// 获取UPS变量列表
std::map<std::string, std::string> NUTClient::get_ups_vars() {
    std::map<std::string, std::string> vars;

    // 获取所有变量
    std::string command = "LIST VAR " + ups_name;
    std::string data = send_command(command);

    DEBUG_PRINT("[NUT] Command: %s\n", command.c_str());
    DEBUG_PRINT("[NUT] Received data: %s\n", data.c_str());

    // 检查错误并打印到控制台，如果有错误则直接返回空map
    if (is_error_response(data)) {
        check_and_print_error(data);
        return vars; // 返回空map作为默认结果
    }

    size_t start = 0;
    size_t length = data.length();
    const char* buffer = data.c_str();
    char ups_name_buf[256], var_name_buf[256], value_buf[256];

    while (start < length) {
        // 解析变量行: "VAR ups_name variable_name "value""
        // 使用 sscanf 解析响应参数, 提取 ups_name, var_name 和 value
        int ret = sscanf(buffer + start, "VAR %255s %255s \"%255[^\n\"]\"", ups_name_buf, var_name_buf, value_buf);
        if (ret == 3) {
            vars[var_name_buf] = value_buf;
        }
        while (start < length && buffer[start] != '\n') {
            start++;
        }
        start++; // next line
    }

    return vars;
}

// 获取特定UPS变量
std::string NUTClient::get_ups_var(const std::string& var_name) {
    std::string command = "GET VAR " + ups_name + " " + var_name;
    std::string response = send_command(command);

    DEBUG_PRINT("[NUT] Command: %s\n", command.c_str());
    DEBUG_PRINT("[NUT] Received data: %s\n", response.c_str());

    // 检查错误并打印到控制台，如果有错误则直接返回空字符串
    if (is_error_response(response)) {
        check_and_print_error(response);
        return ""; // 返回空字符串作为默认结果
    }

    // 使用 sscanf 解析响应参数, 提取 ups_name, var_name 和 value
    char ups_name_buf[256], var_name_buf[256], value_buf[256];
    int ret = sscanf(response.c_str(), "VAR %255s %255s \"%255[^\n\"]\"", ups_name_buf, var_name_buf, value_buf);
    if (ret == 3) {
        // std::cout << "[NUT] Parsed data: " << ups_name_buf << ", " << var_name_buf << ", " << value_buf << std::endl;
        return std::string(value_buf);
    }

    return "";
}

// 检查响应是否为错误
bool NUTClient::is_error_response(const std::string& response) {
    return response.substr(0, 4) == "ERR ";
}

// 检查错误返回并打印错误信息到控制台
void NUTClient::check_and_print_error(const std::string& response) {
    // 去掉开头的 "ERR " 关键字
    std::cerr << "NUT Client Error: " << response.substr(4) << std::endl;
}

// 检查是否连接
bool NUTClient::is_connected() const {
    return connected;
}

// 获取服务器信息
std::string NUTClient::get_server_host() const { return server_host; }
int NUTClient::get_server_port() const { return server_port; }
std::string NUTClient::get_ups_name() const { return ups_name; }