#include "server/server.h"
#include "server/file_server.h"
#include <aws/core/Aws.h>

int main() {
    Aws::SDKOptions options;
    Aws::InitAPI(options); // 初始化 AWS/MinIO SDK

    {
        // 文件传输服务器 3线程
        EventLoop loop;
        InetAddress addr(8080, "192.168.122.164");
        FileServer server(&loop, addr, "file-server");
        server.start();
        loop.loop();
    }

    Aws::ShutdownAPI(options); // 关闭 AWS/MinIO SDK
    return 0;
}