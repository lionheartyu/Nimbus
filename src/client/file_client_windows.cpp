#include "file_client_windows.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QCoreApplication>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QDesktopServices>
#include <QUrl>
#include "../../proto/file.pb.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

// 构造函数：初始化界面和控件
FileClientWindow::FileClientWindow(QWidget *parent) : QWidget(parent) {
    setWindowTitle("Nimbus 文件助手");
    setMinimumSize(520, 360);
    resize(640, 480);

    // 标题
    QLabel *title = new QLabel("Nimbus 文件中心", this);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(
        "font-size: 26px; font-weight: bold; color: #1565c0;"
        "padding: 12px 0 18px 0;"
        "letter-spacing: 2px;"
        "border-bottom: 2px solid #90caf9;"
    );

    // 文件路径输入框
    filePathEdit = new QLineEdit(this);
    filePathEdit->setPlaceholderText("请选择要上传的文件...");
    filePathEdit->setMinimumWidth(260);
    filePathEdit->setStyleSheet(
        "QLineEdit {"
        "  border: 2px solid #90caf9;"
        "  border-radius: 8px;"
        "  padding: 6px;"
        "  font-size: 15px;"
        "  background: #e3f2fd;"
        "}"
    );

    // 浏览按钮
    browseBtn = new QPushButton("浏览...", this);
    browseBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #42a5f5;"
        "  color: white;"
        "  font-weight: bold;"
        "  border-radius: 8px;"
        "  padding: 6px 18px;"
        "  font-size: 15px;"
        "  border: none;"
        "}"
        "QPushButton:hover {"
        "  background-color: #1976d2;"
        "}"
    );

    // 上传按钮
    uploadBtn = new QPushButton("上传文件", this);
    uploadBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #43A047;"
        "  color: white;"
        "  font-size: 17px;"
        "  font-weight: bold;"
        "  border-radius: 8px;"
        "  padding: 8px 0;"
        "  border: none;"
        "  margin-top: 10px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #388e3c;"
        "}"
    );
    uploadBtn->setMinimumHeight(36);

    // 新增：列举和下载按钮
    downloadBtn = new QPushButton("下载文件", this);
    listBtn = new QPushButton("查看云端文件", this);

    listBtn->setStyleSheet("background-color: #ffa726; color: white; font-weight: bold; border-radius: 8px; padding: 6px 18px; font-size: 15px; border: none;");
    downloadBtn->setStyleSheet("background-color: #1976d2; color: white; font-weight: bold; border-radius: 8px; padding: 6px 18px; font-size: 15px; border: none;");

    // 进度条
    progressBar = new QProgressBar(this);
    progressBar->setStyleSheet(
        "QProgressBar {"
        "  border: 2px solid #90caf9;"
        "  border-radius: 8px;"
        "  text-align: center;"
        "  background: #e3f2fd;"
        "  font-size: 13px;"
        "}"
        "QProgressBar::chunk {"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "    stop:0 #42a5f5, stop:1 #1976d2);"
        "  border-radius: 8px;"
        "}"
    );
    progressBar->setMinimumHeight(22);

    // 日志输出框
    logEdit = new QTextEdit(this);
    logEdit->setReadOnly(true);
    logEdit->setStyleSheet(
        "QTextEdit {"
        "  background: #f5f5f5;"
        "  font-family: 'Consolas', 'Monospace';"
        "  font-size: 13px;"
        "  border: 2px solid #cfd8dc;"
        "  border-radius: 8px;"
        "  padding: 8px;"
        "}"
    );

    // 文件列表控件
    fileListWidget = new QListWidget(this);
    fileListWidget->setContextMenuPolicy(Qt::CustomContextMenu);

    // 文件选择区布局
    QHBoxLayout *fileLayout = new QHBoxLayout;
    fileLayout->addWidget(filePathEdit, 3);
    fileLayout->addWidget(browseBtn, 1);
    fileLayout->addWidget(downloadBtn, 1);

    // 主界面布局
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(title);
    mainLayout->addSpacing(10);
    mainLayout->addLayout(fileLayout);
    mainLayout->addWidget(uploadBtn);
    mainLayout->addWidget(listBtn);
    mainLayout->addWidget(progressBar);
    mainLayout->addWidget(fileListWidget, 2);
    mainLayout->addWidget(logEdit, 1);
    mainLayout->setSpacing(16);
    mainLayout->setContentsMargins(32, 24, 32, 24);
    setLayout(mainLayout);

    // 绑定按钮事件
    connect(browseBtn, &QPushButton::clicked, this, &FileClientWindow::onBrowse);
    connect(uploadBtn, &QPushButton::clicked, this, &FileClientWindow::onUpload);
    connect(listBtn, &QPushButton::clicked, this, &FileClientWindow::onList);
    connect(downloadBtn, &QPushButton::clicked, this, &FileClientWindow::onDownload);

    connect(fileListWidget, &QListWidget::itemClicked, this, &FileClientWindow::onFileClicked);
    connect(fileListWidget, &QListWidget::customContextMenuRequested, this, &FileClientWindow::onListContextMenu);
}

// 浏览按钮槽函数：弹出文件选择对话框
void FileClientWindow::onBrowse() {
    QString file = QFileDialog::getOpenFileName(this, "选择文件");
    if (!file.isEmpty()) filePathEdit->setText(file);
}

// 上传按钮槽函数：实现文件上传逻辑
void FileClientWindow::onUpload() {
    QString filePath = filePathEdit->text();
    if (filePath.isEmpty()) {
        QMessageBox::warning(this, "提示", "请选择要上传的文件！");
        return;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        logEdit->append("文件打开失败: " + filePath);
        return;
    }
    QFileInfo info(filePath);
    qint64 filesize = file.size();

    // 构造protobuf头（包含文件名和大小）
    FileHeader header;
    header.set_filename(info.fileName().toStdString());
    header.set_filesize(filesize);
    std::string pb_head;
    header.SerializeToString(&pb_head);

    // 连接服务器
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "192.168.122.164", &serv_addr.sin_addr);
    if (::connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        logEdit->append("连接服务器失败！");
        ::close(sock);
        return;
    }

    // 发送4字节长度+protobuf头
    uint32_t pb_len = pb_head.size();
    char len_buf[4];
    memcpy(len_buf, &pb_len, 4);
    send(sock, len_buf, 4, 0);
    send(sock, pb_head.data(), pb_head.size(), 0);

    // 发送文件内容（分块发送，并更新进度条）
    qint64 sent = 0;
    char buf[4096];
    while (!file.atEnd()) {
        qint64 n = file.read(buf, sizeof(buf));
        if (n > 0) {
            send(sock, buf, n, 0);
            sent += n;
            int percent = (filesize > 0) ? (sent * 100 / filesize) : 100;
            progressBar->setValue(percent);
            QCoreApplication::processEvents(); // 保持界面响应
        }
    }
    file.close();

    // 等待服务器响应
    char resp[128] = {0};
    int n = recv(sock, resp, sizeof(resp)-1, 0);
    if (n > 0) {
        logEdit->append("服务器响应: " + QString::fromUtf8(resp));
    } else {
        logEdit->append("未收到服务器响应！");
    }
    ::close(sock);
    progressBar->setValue(100);
}

// 新增：列举按钮槽函数，实现文件列举逻辑
void FileClientWindow::onList() {
    // 构造 type=3 的 protobuf 头
    FileHeader header;
    header.set_type(3);
    std::string pb_head;
    header.SerializeToString(&pb_head);

    // 连接服务器
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "192.168.122.164", &serv_addr.sin_addr);
    if (::connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        logEdit->append("连接服务器失败！");
        ::close(sock);
        return;
    }

    // 发送4字节长度+protobuf头
    uint32_t pb_len = pb_head.size();
    char len_buf[4];
    memcpy(len_buf, &pb_len, 4);
    send(sock, len_buf, 4, 0);
    send(sock, pb_head.data(), pb_head.size(), 0);

    // 接收4字节长度
    uint32_t resp_len = 0;
    int n = recv(sock, &resp_len, 4, MSG_WAITALL);
    if (n != 4) {
        logEdit->append("未收到服务器响应长度！");
        ::close(sock);
        return;
    }

    // 接收protobuf内容
    std::string resp_buf(resp_len, '\0');
    n = recv(sock, &resp_buf[0], resp_len, MSG_WAITALL);
    if (n != (int)resp_len) {
        logEdit->append("未收到完整文件列表！");
        ::close(sock);
        return;
    }

    // 解析并显示
    ListFilesResponse resp;
    if (!resp.ParseFromString(resp_buf)) {
        logEdit->append("文件列表解析失败！");
        ::close(sock);
        return;
    }

    // === 新增：每次只显示最新云盘文件列表 ===
    logEdit->clear();
    logEdit->append("云盘文件列表：");
    for (int i = 0; i < resp.filenames_size(); ++i) {
        logEdit->append(QString::fromStdString(resp.filenames(i)));
    }
    ::close(sock);

    // 更新文件列表控件
    fileListWidget->clear();
    for (int i = 0; i < resp.filenames_size(); ++i) {
        fileListWidget->addItem(QString::fromStdString(resp.filenames(i)));
    }
}

// 新增：下载按钮槽函数，实现文件下载逻辑
void FileClientWindow::onDownload() {
    QString filename = filePathEdit->text().trimmed();
    if (filename.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入要下载的文件名！");
        return;
    }

    // 构造 type=2 的 protobuf 头
    FileHeader header;
    header.set_filename(filename.toStdString());
    header.set_type(2);
    std::string pb_head;
    header.SerializeToString(&pb_head);

    // 连接服务器
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "192.168.122.164", &serv_addr.sin_addr);
    if (::connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        logEdit->append("连接服务器失败！");
        ::close(sock);
        return;
    }

    // 发送4字节长度+protobuf头
    uint32_t pb_len = pb_head.size();
    char len_buf[4];
    memcpy(len_buf, &pb_len, 4);
    send(sock, len_buf, 4, 0);
    send(sock, pb_head.data(), pb_head.size(), 0);

    // 接收4字节文件长度
    uint32_t file_len = 0;
    int n = recv(sock, &file_len, 4, MSG_WAITALL);
    if (n != 4) {
        logEdit->append("未收到服务器文件长度！");
        ::close(sock);
        return;
    }

    // 接收文件内容
    std::vector<char> file_buf(file_len);
    size_t received = 0;
    while (received < file_len) {
        int chunk = recv(sock, &file_buf[received], file_len - received, 0);
        if (chunk <= 0) break;
        received += chunk;
        int percent = (file_len > 0) ? (received * 100 / file_len) : 100;
        progressBar->setValue(percent);
        QCoreApplication::processEvents();
    }
    ::close(sock);

    if (received != file_len) {
        logEdit->append("文件接收不完整！");
        return;
    }

    // 保存为本地文件
    QString savePath = QFileDialog::getSaveFileName(this, "保存文件", filename);
    if (savePath.isEmpty()) return;
    QFile outFile(savePath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        logEdit->append("保存文件失败: " + savePath);
        return;
    }
    outFile.write(file_buf.data(), file_buf.size());
    outFile.close();
    logEdit->append("下载完成，已保存为: " + savePath);
    progressBar->setValue(100);
}

// 文件列表项点击事件：更新文件路径输入框
void FileClientWindow::onFileClicked(QListWidgetItem *item) {
    if (item) {
        filePathEdit->setText(item->text());
    }
}

// 文件列表右键菜单事件：提供下载、查看、删除等选项
void FileClientWindow::onListContextMenu(const QPoint &pos) {
    QListWidgetItem *item = fileListWidget->itemAt(pos);
    if (!item) return;

    QMenu contextMenu;
    QAction *downloadAction = contextMenu.addAction("下载文件");
    QAction *viewAction = contextMenu.addAction("查看文件");
    QAction *deleteAction = contextMenu.addAction("删除(移入回收站)");

    QAction *selectedAction = contextMenu.exec(fileListWidget->viewport()->mapToGlobal(pos));
    if (selectedAction == downloadAction) {
        filePathEdit->setText(item->text());
        onDownload();
    } else if (selectedAction == viewAction) {
        QString filename = item->text();
        QString fileUrl = "http://192.168.122.164:8080/files/" + filename;
        QDesktopServices::openUrl(QUrl(fileUrl));
    } else if (selectedAction == deleteAction) {
        FileHeader header;
        header.set_filename(item->text().toStdString());
        header.set_type(4);
        std::string pb_head;
        header.SerializeToString(&pb_head);

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(8080);
        inet_pton(AF_INET, "192.168.122.164", &serv_addr.sin_addr);
        if (::connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            logEdit->append("连接服务器失败！");
            ::close(sock);
            return;
        }
        uint32_t pb_len = pb_head.size();
        char len_buf[4];
        memcpy(len_buf, &pb_len, 4);
        send(sock, len_buf, 4, 0);
        send(sock, pb_head.data(), pb_head.size(), 0);

        char resp[128] = {0};
        int n = recv(sock, resp, sizeof(resp)-1, 0);
        if (n > 0) {
            logEdit->append("服务器响应: " + QString::fromUtf8(resp));
        } else {
            logEdit->append("未收到服务器响应！");
        }
        ::close(sock);
        onList();
    }
}