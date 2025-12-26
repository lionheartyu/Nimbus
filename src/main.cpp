#include "server/file_server.h"
#include <aws/core/Aws.h>

int main() {
    // 初始化 AWS/MinIO SDK
    Aws::SDKOptions options;
    Aws::InitAPI(options);

    {
        // 创建事件循环和服务器对象，监听 10.20.32.88:8080，3线程
        EventLoop loop;
        InetAddress addr(8080, "10.20.32.88");
        FileServer server(&loop, addr, "file-server");
        server.start();
        loop.loop(); // 启动事件循环
    }

    // 关闭 AWS/MinIO SDK
    Aws::ShutdownAPI(options);
    return 0;
}