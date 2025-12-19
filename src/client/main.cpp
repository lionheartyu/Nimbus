#include <QApplication>
#include "file_client_windows.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    FileClientWindow window;
    window.setWindowTitle("NimBus");
    window.show();
    return app.exec();
}