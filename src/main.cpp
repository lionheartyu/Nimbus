#include"server/server.h"
#include"server/file_server.h"
int main(){
    //文件传输服务器 3线程
    EventLoop loop;
    InetAddress addr(8080, "10.20.32.132");
    FileServer server(&loop, addr, "file-server");
    server.start();
    loop.loop();
    return 0;
}