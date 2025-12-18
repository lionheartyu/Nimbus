#include <iostream>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <cstring>
#include <iomanip>
#include "../../proto/file.pb.h"  // 路径按实际调整

int main() {
    const char* server_ip = "192.168.122.164";
    int server_port = 8080;
    std::string filename;

    std::cout << "请输入要上传的文件名: ";
    std::getline(std::cin, filename);

    // 读取文件内容
    std::ifstream infile(filename, std::ios::binary | std::ios::ate);
    if (!infile) {
        std::cerr << "Cannot open file: " << filename << std::endl;
        return 1;
    }
    std::streamsize filesize = infile.tellg();
    infile.seekg(0, std::ios::beg);
    std::vector<char> filedata(filesize);
    if (!infile.read(filedata.data(), filesize)) {
        std::cerr << "Read file error." << std::endl;
        return 1;
    }

    // 创建socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Socket creation failed." << std::endl;
        return 1;
    }

    sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &serv_addr.sin_addr);

    if (connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "Connect failed." << std::endl;
        close(sock);
        return 1;
    }

    // 构造protobuf文件头
    FileHeader header;
    header.set_filename(filename);
    header.set_filesize(filesize);
    std::string pb_head;
    header.SerializeToString(&pb_head);

    // 发送4字节protobuf头长度
    uint32_t pb_head_len = pb_head.size();
    send(sock, &pb_head_len, 4, 0);

    // 发送protobuf头
    size_t sent = 0;
    while (sent < pb_head.size()) {
        ssize_t n = send(sock, pb_head.data() + sent, pb_head.size() - sent, 0);
        if (n <= 0) {
            std::cerr << "Send protobuf header failed." << std::endl;
            close(sock);
            return 1;
        }
        sent += n;
    }

    // 发送文件内容并显示进度
    sent = 0;
    const size_t chunk_size = 4096;
    while (sent < filedata.size()) {
        size_t to_send = std::min(chunk_size, filedata.size() - sent);
        ssize_t n = send(sock, filedata.data() + sent, to_send, 0);
        if (n <= 0) {
            std::cerr << "Send file failed." << std::endl;
            close(sock);
            return 1;
        }
        sent += n;
        // 显示进度
        double percent = 100.0 * sent / filedata.size();
        std::cout << "\r已发送: " << sent << "/" << filedata.size()
                  << " 字节 (" << std::fixed << std::setprecision(2) << percent << "%)" << std::flush;
    }
    std::cout << std::endl;

    // 接收服务器响应
    char buf[1024] = {0};
    ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        std::cout << "Server: " << buf << std::endl;
    }

    close(sock);
    return 0;
}