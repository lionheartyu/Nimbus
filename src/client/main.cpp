#include <QApplication>
#include "file_client_windows.h"
#include "login_dialog.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    LoginDialog login;
    if (login.exec() != QDialog::Accepted)
        return 0;

    FileClientWindow window;
    window.setWindowTitle("NimBus");
    window.setToken(login.token());
    window.show();

    return app.exec();
}