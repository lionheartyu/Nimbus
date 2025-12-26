#include <QApplication>
#include "file_client_windows.h"
#include "login_dialog.h"

int main(int argc, char *argv[])
{
    // 创建 Qt 应用程序对象
    QApplication app(argc, argv);

    // 弹出登录对话框，未登录直接退出
    LoginDialog login;
    if (login.exec() != QDialog::Accepted)
        return 0;

    // 登录成功后，创建主窗口并传递 token
    FileClientWindow window;
    window.setWindowTitle("NimBus");
    window.setToken(login.token());
    window.show();

    // 进入 Qt 事件循环
    return app.exec();
}