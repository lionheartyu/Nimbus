#include "file_client_windows.h"
#include "draggable_listwidget.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "../../proto/file.pb.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

// 避免 UI 卡死：给 socket 设置读写超时（秒）
constexpr int kSockTimeoutSec = 10;

// 防止服务端/网络异常导致分配超大内存或一直等
constexpr uint32_t kMaxListResponseBytes = 8u * 1024u * 1024u;     // 8MB 足够装文件名列表
constexpr uint32_t kMaxDownloadBytes     = 1024u * 1024u * 1024u;  // 1GB（按需调整）

bool setSocketTimeouts(int sock, int seconds)
{
    timeval tv{};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return false;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return false;
    return true;
}

// 处理短写
bool sendAll(int sock, const void* data, size_t len)
{
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;

    while (sent < len)
    {
        ssize_t n = ::send(sock, p + sent, len - sent, 0);
        if (n > 0)
        {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

// 处理短读（不使用 MSG_WAITALL，避免在部分 TCP 栈/超时下怪行为）
bool recvAll(int sock, void* data, size_t len)
{
    char* p = static_cast<char*>(data);
    size_t recvd = 0;

    while (recvd < len)
    {
        ssize_t n = ::recv(sock, p + recvd, len - recvd, 0);
        if (n > 0)
        {
            recvd += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) return false;  // 对端关闭
        if (errno == EINTR) continue;
        return false;  // 超时/连接错误等
    }
    return true;
}

bool connectToServer(int& sock, const char* ip, uint16_t port)
{
    sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    setSocketTimeouts(sock, kSockTimeoutSec);

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) != 1)
    {
        ::close(sock);
        sock = -1;
        return false;
    }

    if (::connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        ::close(sock);
        sock = -1;
        return false;
    }
    return true;
}

bool sendPbHeader(int sock, const std::string& pb)
{
    uint32_t pb_len = static_cast<uint32_t>(pb.size());
    // 注意：你服务端目前显然按“小端/原样 uint32”读，这里保持一致（不做 htonl）
    return sendAll(sock, &pb_len, sizeof(pb_len)) && sendAll(sock, pb.data(), pb.size());
}

}  // namespace

// 构造函数：初始化界面和控件
FileClientWindow::FileClientWindow(QWidget* parent)
    : QWidget(parent)
{
    setWindowTitle("Nimbus 文件助手");
    // 更接近网盘客户端尺寸
    setMinimumSize(820, 560);
    resize(980, 680);

    // 整体背景更干净
    setStyleSheet("background:#ffffff;");

    // 标题（更克制、接近网盘风）
    QLabel* title = new QLabel("Nimbus 客户端", this);
    title->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    title->setStyleSheet(
        "QLabel {"
        "  font-size: 18px;"
        "  font-weight: 700;"
        "  color: #1f2937;"
        "  padding: 10px 12px;"
        "  border-bottom: 1px solid #e5e7eb;"
        "}");

    // 统一按钮样式（扁平 + 圆角 + hover/pressed）
    const QString primaryBtn =
        "QPushButton{"
        "  background:#2d77ff;"
        "  color:#ffffff;"
        "  border:none;"
        "  border-radius:8px;"
        "  padding:8px 14px;"
        "  font-size:14px;"
        "  font-weight:600;"
        "}"
        "QPushButton:hover{ background:#2563eb; }"
        "QPushButton:pressed{ background:#1d4ed8; }"
        "QPushButton:disabled{ background:#9ca3af; color:#f3f4f6; }";

    const QString lightBtn =
        "QPushButton{"
        "  background:#f3f4f6;"
        "  color:#111827;"
        "  border:1px solid #e5e7eb;"
        "  border-radius:8px;"
        "  padding:8px 14px;"
        "  font-size:14px;"
        "  font-weight:600;"
        "}"
        "QPushButton:hover{ background:#e5e7eb; }"
        "QPushButton:pressed{ background:#d1d5db; }"
        "QPushButton:disabled{ color:#9ca3af; }";

    const QString warnBtn =
        "QPushButton{"
        "  background:#fff7ed;"
        "  color:#9a3412;"
        "  border:1px solid #fed7aa;"
        "  border-radius:8px;"
        "  padding:8px 14px;"
        "  font-size:14px;"
        "  font-weight:600;"
        "}"
        "QPushButton:hover{ background:#ffedd5; }"
        "QPushButton:pressed{ background:#fed7aa; }";

    // 文件路径输入框（更像网盘搜索/路径框）
    filePathEdit = new QLineEdit(this);
    filePathEdit->setPlaceholderText("选择文件或输入要下载的文件名…");
    filePathEdit->setMinimumHeight(36);
    filePathEdit->setStyleSheet(
        "QLineEdit{"
        "  border:1px solid #e5e7eb;"
        "  border-radius:10px;"
        "  padding:8px 12px;"
        "  font-size:14px;"
        "  background:#ffffff;"
        "  color:#111827;"
        "}"
        "QLineEdit:focus{"
        "  border:1px solid #2d77ff;"
        "}");

    browseBtn = new QPushButton("选择文件", this);
    browseBtn->setMinimumHeight(36);
    browseBtn->setStyleSheet(lightBtn);

    uploadBtn = new QPushButton("上传", this);
    uploadBtn->setMinimumHeight(36);
    uploadBtn->setStyleSheet(primaryBtn);

    downloadBtn = new QPushButton("下载", this);
    downloadBtn->setMinimumHeight(36);
    downloadBtn->setStyleSheet(lightBtn);

    listBtn = new QPushButton("云端文件", this);
    listBtn->setMinimumHeight(36);
    listBtn->setStyleSheet(lightBtn);

    recycleBtn = new QPushButton("回收站", this);
    recycleBtn->setMinimumHeight(36);
    recycleBtn->setStyleSheet(warnBtn);

    // 进度条（更细更现代）
    progressBar = new QProgressBar(this);
    progressBar->setMinimumHeight(12);
    progressBar->setTextVisible(true);
    progressBar->setStyleSheet(
        "QProgressBar{"
        "  border:1px solid #e5e7eb;"
        "  border-radius:6px;"
        "  background:#f3f4f6;"
        "  text-align:center;"
        "  font-size:12px;"
        "  color:#374151;"
        "}"
        "QProgressBar::chunk{"
        "  background:#2d77ff;"
        "  border-radius:6px;"
        "}");

    // 日志（不再挤成一小条）
    logEdit = new QTextEdit(this);
    logEdit->setReadOnly(true);
    logEdit->setStyleSheet(
        "QTextEdit{"
        "  background:#ffffff;"
        "  border:1px solid #e5e7eb;"
        "  border-radius:10px;"
        "  padding:10px;"
        "  font-family: 'Consolas','Monospace';"
        "  font-size:12px;"
        "  color:#374151;"
        "}");
    logEdit->setMinimumHeight(80);
    logEdit->setMaximumHeight(120);

    // 文件列表控件（边框更轻）
    fileListWidget = new DraggableListWidget(this);
    static_cast<DraggableListWidget*>(fileListWidget)->setMainWindow(this);
    fileListWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    fileListWidget->setAcceptDrops(true);
    fileListWidget->setDragDropMode(QAbstractItemView::DropOnly);
    fileListWidget->setStyleSheet(
        "QListWidget{"
        "  border:1px solid #e5e7eb;"
        "  border-radius:10px;"
        "  background:#ffffff;"
        "  padding:6px;"
        "  font-size:14px;"
        "}"
        "QListWidget::item{"
        "  padding:10px 10px;"
        "  border-radius:8px;"
        "}"
        "QListWidget::item:selected{"
        "  background:#e8f0ff;"
        "  color:#111827;"
        "}");

    // 文件选择区布局（更宽）
    QHBoxLayout* fileLayout = new QHBoxLayout;
    fileLayout->addWidget(filePathEdit, 1);
    fileLayout->addWidget(browseBtn, 0);
    fileLayout->setSpacing(10);

    // 顶部操作区：更像网盘“工具条”
    QHBoxLayout* toolbarLayout = new QHBoxLayout;
    toolbarLayout->addWidget(uploadBtn, 0);
    toolbarLayout->addWidget(downloadBtn, 0);
    toolbarLayout->addStretch(1);
    toolbarLayout->addWidget(listBtn, 0);
    toolbarLayout->addWidget(recycleBtn, 0);
    toolbarLayout->setSpacing(10);

    // 主界面布局
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(title);
    mainLayout->addSpacing(8);
    mainLayout->addLayout(fileLayout);
    mainLayout->addLayout(toolbarLayout);
    mainLayout->addWidget(progressBar);
    mainLayout->addWidget(fileListWidget, 1);
    mainLayout->addWidget(logEdit, 0);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(18, 14, 18, 16);
    setLayout(mainLayout);

    // 绑定按钮事件
    connect(browseBtn, &QPushButton::clicked, this, &FileClientWindow::onBrowse);
    connect(uploadBtn, &QPushButton::clicked, this, &FileClientWindow::onUpload);
    connect(listBtn, &QPushButton::clicked, this, &FileClientWindow::onList);
    connect(downloadBtn, &QPushButton::clicked, this, &FileClientWindow::onDownload);
    connect(recycleBtn, &QPushButton::clicked, this, &FileClientWindow::onRecycle);

    connect(fileListWidget, &QListWidget::itemClicked, this, &FileClientWindow::onFileClicked);
    connect(fileListWidget,
            &QListWidget::customContextMenuRequested,
            this,
            &FileClientWindow::onListContextMenu);
    connect(fileListWidget, &QListWidget::itemDoubleClicked, this, &FileClientWindow::onFileDoubleClicked);
}

// 浏览按钮槽函数：弹出文件选择对话框
void FileClientWindow::onBrowse()
{
    const QString file = QFileDialog::getOpenFileName(this, "选择文件");
    if (!file.isEmpty())
        filePathEdit->setText(file);
}

// 上传按钮槽函数：实现文件上传逻辑
void FileClientWindow::onUpload()
{
    const QString filePath = filePathEdit->text();
    if (filePath.isEmpty())
    {
        QMessageBox::warning(this, "提示", "请选择要上传的文件！");
        return;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        logEdit->append("文件打开失败: " + filePath);
        return;
    }

    const QFileInfo info(filePath);
    const qint64 filesize = file.size();

    const QString relPath = info.fileName();
    FileHeader header;
    header.set_filename(relPath.toStdString());
    header.set_filesize(filesize);

    std::string pb_head;
    header.SerializeToString(&pb_head);

    int sock = -1;
    if (!connectToServer(sock, "10.20.32.88", 8080))
    {
        logEdit->append("连接服务器失败！");
        return;
    }

    // 发送 4字节长度 + protobuf 头（用可靠发送）
    const uint32_t pb_len = static_cast<uint32_t>(pb_head.size());
    if (!sendAll(sock, &pb_len, sizeof(pb_len)) || !sendAll(sock, pb_head.data(), pb_head.size()))
    {
        logEdit->append("发送上传头失败（可能超时/断开）！");
        ::close(sock);
        return;
    }

    // 发送文件内容（分块发送，并更新进度条）
    qint64 sent = 0;
    char buf[4096];
    while (!file.atEnd())
    {
        const qint64 n = file.read(buf, sizeof(buf));
        if (n > 0)
        {
            if (!sendAll(sock, buf, static_cast<size_t>(n)))
            {
                logEdit->append("上传中断（可能超时/断开）！");
                ::close(sock);
                return;
            }
            sent += n;
            const int percent = (filesize > 0) ? (sent * 100 / filesize) : 100;
            progressBar->setValue(percent);
            QCoreApplication::processEvents();
        }
    }
    file.close();

    // 等待服务器响应
    char resp[128] = {0};
    const int rn = ::recv(sock, resp, sizeof(resp) - 1, 0);
    ::close(sock);

    const QString respText = (rn > 0) ? QString::fromUtf8(resp).trimmed() : QString();

    if (rn > 0)
    {
        logEdit->append("服务器响应: " + respText);

        // 成功弹窗
        if (respText.startsWith("UPLOAD OK", Qt::CaseInsensitive))
        {
            QMessageBox::information(this, "上传成功", "文件上传成功：\n" + relPath);
        }
    }
    else
    {
        logEdit->append("未收到服务器响应（可能超时）！");
    }

    progressBar->setValue(100);

    QTimer::singleShot(300, this, [this]() {
        if (!inRecycle) onList();
    });
}

// 新增：列举按钮槽函数，实现文件列举逻辑
void FileClientWindow::onList()
{
    FileHeader header;
    header.set_type(3);

    std::string pb_head;
    header.SerializeToString(&pb_head);

    int sock = -1;
    if (!connectToServer(sock, "10.20.32.88", 8080))
    {
        logEdit->append("连接服务器失败！");
        return;
    }

    if (!sendPbHeader(sock, pb_head))
    {
        logEdit->append("发送请求失败！");
        ::close(sock);
        return;
    }

    uint32_t resp_len = 0;
    if (!recvAll(sock, &resp_len, sizeof(resp_len)))
    {
        logEdit->append("未收到服务器响应长度（可能超时）！");
        ::close(sock);
        return;
    }

    if (resp_len == 0)
    {
        // 空列表：不要卡死，直接刷新为空
        fileListWidget->clear();
        logEdit->clear();
        logEdit->append("云盘文件列表为空。");
        ::close(sock);
        inRecycle = false;
        return;
    }

    if (resp_len > kMaxListResponseBytes)
    {
        logEdit->append("服务器返回的列表长度异常，已拒绝处理。");
        ::close(sock);
        return;
    }

    std::string resp_buf(resp_len, '\0');
    if (!recvAll(sock, &resp_buf[0], resp_len))
    {
        logEdit->append("未收到完整文件列表（可能超时/断开）！");
        ::close(sock);
        return;
    }

    ListFilesResponse resp;
    if (!resp.ParseFromString(resp_buf))
    {
        logEdit->append("文件列表解析失败！");
        ::close(sock);
        return;
    }

    logEdit->clear();
    logEdit->append("云盘文件列表已刷新。");
    fileListWidget->clear();

    for (int i = 0; i < resp.filenames_size(); ++i)
    {
        const QString name = QString::fromStdString(resp.filenames(i));
        if (!name.startsWith("recycle/"))
        {
            fileListWidget->addItem(name);
        }
    }

    ::close(sock);
    inRecycle = false;
}

// 新增：下载按钮槽函数，实现文件下载逻辑
void FileClientWindow::onDownload()
{
    const QString filename = filePathEdit->text().trimmed();
    if (filename.isEmpty())
    {
        QMessageBox::warning(this, "提示", "请输入要下载的文件名！");
        return;
    }

    FileHeader header;
    header.set_filename(filename.toStdString());
    header.set_type(2);

    std::string pb_head;
    header.SerializeToString(&pb_head);

    int sock = -1;
    if (!connectToServer(sock, "10.20.32.88", 8080))
    {
        logEdit->append("连接服务器失败！");
        return;
    }

    if (!sendPbHeader(sock, pb_head))
    {
        logEdit->append("发送下载请求失败！");
        ::close(sock);
        return;
    }

    uint32_t file_len = 0;
    if (!recvAll(sock, &file_len, sizeof(file_len)))
    {
        logEdit->append("未收到服务器文件长度（可能超时）！");
        ::close(sock);
        return;
    }

    if (file_len == 0)
    {
        logEdit->append("文件为空或服务器返回长度为0。");
        ::close(sock);
        progressBar->setValue(0);
        return;
    }

    if (file_len > kMaxDownloadBytes)
    {
        logEdit->append("文件过大或长度异常，已拒绝下载。");
        ::close(sock);
        progressBar->setValue(0);
        return;
    }

    const QString savePath = QFileDialog::getSaveFileName(this, "保存文件", filename);
    if (savePath.isEmpty())
    {
        ::close(sock);
        return;
    }

    QFile outFile(savePath);
    if (!outFile.open(QIODevice::WriteOnly))
    {
        logEdit->append("保存文件失败: " + savePath);
        ::close(sock);
        return;
    }

    progressBar->setValue(0);

    uint32_t received = 0;
    std::vector<char> buf(64 * 1024);

    while (received < file_len)
    {
        const uint32_t want = std::min<uint32_t>(static_cast<uint32_t>(buf.size()), file_len - received);
        const ssize_t n = ::recv(sock, buf.data(), want, 0);

        if (n > 0)
        {
            outFile.write(buf.data(), n);
            received += static_cast<uint32_t>(n);

            const int percent = (file_len > 0) ? (static_cast<uint64_t>(received) * 100 / file_len) : 100;
            progressBar->setValue(percent);
            QCoreApplication::processEvents();
            continue;
        }

        outFile.close();
        ::close(sock);
        logEdit->append("下载中断（可能超时/断开）。已接收 " + QString::number(received) + " 字节。");
        progressBar->setValue(0);
        return;
    }

    outFile.close();
    ::close(sock);

    logEdit->append("下载完成，已保存为: " + savePath);
    progressBar->setValue(100);

    // 成功弹窗
    QMessageBox::information(this, "下载成功", "文件下载成功：\n" + savePath);
}

// 文件列表项点击事件：更新文件路径输入框
void FileClientWindow::onFileClicked(QListWidgetItem* item)
{
    if (item)
    {
        filePathEdit->setText(item->text());
    }
}

// 文件列表右键菜单事件：提供下载、查看、删除等选项
void FileClientWindow::onListContextMenu(const QPoint& pos)
{
    QListWidgetItem* item = fileListWidget->itemAt(pos);
    if (!item) return;

    QMenu contextMenu;
    QAction* downloadAction = contextMenu.addAction("下载文件");
    QAction* viewAction = contextMenu.addAction("查看文件");
    QAction* deleteAction = nullptr;
    QAction* restoreAction = nullptr;
    QAction* removeAction = nullptr;

    if (!inRecycle)
    {
        deleteAction = contextMenu.addAction("删除(移入回收站)");
    }
    else
    {
        restoreAction = contextMenu.addAction("还原");
        removeAction = contextMenu.addAction("彻底删除");
    }

    QAction* selectedAction = contextMenu.exec(fileListWidget->viewport()->mapToGlobal(pos));
    const QString filename = item->text();

    if (selectedAction == downloadAction)
    {
        filePathEdit->setText(filename);
        onDownload();
        return;
    }

    if (selectedAction == viewAction)
    {
        const QString fileUrl = "http://10.20.32.88:8080/files/" + filename;
        QDesktopServices::openUrl(QUrl(fileUrl));
        return;
    }

    if (selectedAction == deleteAction)
    {
        FileHeader header;
        header.set_filename(filename.toStdString());
        header.set_type(4);

        std::string pb_head;
        header.SerializeToString(&pb_head);

        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(8080);
        inet_pton(AF_INET, "10.20.32.88", &serv_addr.sin_addr);

        if (::connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        {
            logEdit->append("连接服务器失败！");
            ::close(sock);
            return;
        }

        const uint32_t pb_len = static_cast<uint32_t>(pb_head.size());
        char len_buf[4];
        memcpy(len_buf, &pb_len, 4);

        send(sock, len_buf, 4, 0);
        send(sock, pb_head.data(), pb_head.size(), 0);

        char resp[128] = {0};
        const int n = recv(sock, resp, sizeof(resp) - 1, 0);
        const QString respText = (n > 0) ? QString::fromUtf8(resp).trimmed() : QString();

        if (n > 0)
        {
            logEdit->append("服务器响应: " + respText);

            // 成功弹窗（按你服务端返回 "DELETE OK"）
            if (respText.startsWith("DELETE OK", Qt::CaseInsensitive))
            {
                QMessageBox::information(this, "删除成功", "已删除并移入回收站：\n" + filename);
            }
        }
        else
        {
            logEdit->append("未收到服务器响应！");
        }

        ::close(sock);
        onList();
        return;
    }

    if (selectedAction == restoreAction)
    {
        // 还原
        FileHeader header;
        header.set_filename(filename.toStdString());
        header.set_type(6);

        std::string pb_head;
        header.SerializeToString(&pb_head);

        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(8080);
        inet_pton(AF_INET, "10.20.32.88", &serv_addr.sin_addr);

        if (::connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        {
            logEdit->append("连接服务器失败！");
            ::close(sock);
            return;
        }

        const uint32_t pb_len = static_cast<uint32_t>(pb_head.size());
        char len_buf[4];
        memcpy(len_buf, &pb_len, 4);

        send(sock, len_buf, 4, 0);
        send(sock, pb_head.data(), pb_head.size(), 0);

        char resp[128] = {0};
        const int n = recv(sock, resp, sizeof(resp) - 1, 0);
        if (n > 0)
        {
            logEdit->append("服务器响应: " + QString::fromUtf8(resp));
        }
        else
        {
            logEdit->append("未收到服务器响应！");
        }
        ::close(sock);
        onRecycle();
        return;
    }

    if (selectedAction == removeAction)
    {
        // 彻底删除
        FileHeader header;
        header.set_filename(filename.toStdString());
        header.set_type(7);

        std::string pb_head;
        header.SerializeToString(&pb_head);

        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(8080);
        inet_pton(AF_INET, "10.20.32.88", &serv_addr.sin_addr);

        if (::connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        {
            logEdit->append("连接服务器失败！");
            ::close(sock);
            return;
        }

        const uint32_t pb_len = static_cast<uint32_t>(pb_head.size());
        char len_buf[4];
        memcpy(len_buf, &pb_len, 4);

        send(sock, len_buf, 4, 0);
        send(sock, pb_head.data(), pb_head.size(), 0);

        char resp[128] = {0};
        const int n = recv(sock, resp, sizeof(resp) - 1, 0);
        if (n > 0)
        {
            logEdit->append("服务器响应: " + QString::fromUtf8(resp));
        }
        else
        {
            logEdit->append("未收到服务器响应！");
        }
        ::close(sock);
        onRecycle();
        return;
    }
}

// 递归上传目录下所有文件
void FileClientWindow::uploadDirectory(const QString& rootDir, const QString& currentDir)
{
    QDir dir(currentDir);
    QFileInfoList fileList =
        dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::AllDirs, QDir::DirsFirst);

    for (const QFileInfo& info : fileList)
    {
        if (info.isDir())
        {
            uploadDirectory(rootDir, info.absoluteFilePath());
        }
        else if (info.isFile())
        {
            const QString relPath = QDir(rootDir).relativeFilePath(info.absoluteFilePath());
            uploadFileWithRelativePath(info.absoluteFilePath(), relPath);
        }
    }
}

// 上传单个文件，filename字段为相对路径
void FileClientWindow::uploadFileWithRelativePath(const QString& absPath, const QString& relPath)
{
    QFile file(absPath);
    if (!file.open(QIODevice::ReadOnly))
    {
        logEdit->append("文件打开失败: " + absPath);
        return;
    }

    const qint64 filesize = file.size();

    FileHeader header;
    header.set_filename(relPath.toStdString());
    header.set_filesize(filesize);

    std::string pb_head;
    header.SerializeToString(&pb_head);

    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "10.20.32.88", &serv_addr.sin_addr);

    if (::connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        logEdit->append("连接服务器失败！");
        ::close(sock);
        return;
    }

    const uint32_t pb_len = static_cast<uint32_t>(pb_head.size());
    char len_buf[4];
    memcpy(len_buf, &pb_len, 4);

    send(sock, len_buf, 4, 0);
    send(sock, pb_head.data(), pb_head.size(), 0);

    char buf[4096];
    while (!file.atEnd())
    {
        const qint64 n = file.read(buf, sizeof(buf));
        if (n > 0)
        {
            send(sock, buf, n, 0);
        }
    }
    file.close();

    char resp[128] = {0};
    const int n = recv(sock, resp, sizeof(resp) - 1, 0);

    if (n > 0)
    {
        logEdit->append("上传: " + relPath + " -> " + QString::fromUtf8(resp));
    }
    else
    {
        logEdit->append("上传: " + relPath + " 未收到服务器响应！");
    }

    ::close(sock);
}

// 新增：回收站按钮槽函数，实现回收站文件列举逻辑
void FileClientWindow::onRecycle()
{
    FileHeader header;
    header.set_type(5);

    std::string pb_head;
    header.SerializeToString(&pb_head);

    int sock = -1;
    if (!connectToServer(sock, "10.20.32.88", 8080))
    {
        logEdit->append("连接服务器失败！");
        return;
    }

    if (!sendPbHeader(sock, pb_head))
    {
        logEdit->append("发送请求失败！");
        ::close(sock);
        return;
    }

    uint32_t resp_len = 0;
    if (!recvAll(sock, &resp_len, sizeof(resp_len)))
    {
        logEdit->append("未收到服务器响应长度（可能超时）！");
        ::close(sock);
        return;
    }

    if (resp_len == 0)
    {
        fileListWidget->clear();
        logEdit->clear();
        logEdit->append("回收站为空。");
        ::close(sock);
        inRecycle = true;
        return;
    }

    if (resp_len > kMaxListResponseBytes)
    {
        logEdit->append("服务器返回的回收站列表长度异常，已拒绝处理。");
        ::close(sock);
        return;
    }

    std::string resp_buf(resp_len, '\0');
    if (!recvAll(sock, &resp_buf[0], resp_len))
    {
        logEdit->append("未收到完整回收站列表（可能超时/断开）！");
        ::close(sock);
        return;
    }

    ListFilesResponse resp;
    if (!resp.ParseFromString(resp_buf))
    {
        logEdit->append("回收站列表解析失败！");
        ::close(sock);
        return;
    }

    fileListWidget->clear();
    logEdit->clear();
    logEdit->append("回收站文件列表已刷新。");

    for (int i = 0; i < resp.filenames_size(); ++i)
    {
        QString name = QString::fromStdString(resp.filenames(i));
        if (name.startsWith("recycle/"))
        {
            name = name.mid(QString("recycle/").size());
            fileListWidget->addItem(name);
        }
    }

    ::close(sock);
    inRecycle = true;  // 标记为回收站模式
}

// 文件列表双击事件：打开文件
void FileClientWindow::onFileDoubleClicked(QListWidgetItem* item)
{
    if (!item) return;

    QString filename = item->text();
    if (inRecycle) filename = "recycle/" + filename;

    // 1. 发送 type=8 请求到服务端，获取 presigned url
    FileHeader header;
    header.set_filename(filename.toStdString());
    header.set_type(8);  // 8 表示获取外链

    std::string pb_head;
    header.SerializeToString(&pb_head);

    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "10.20.32.88", &serv_addr.sin_addr);

    if (::connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        logEdit->append("连接服务器失败！");
        ::close(sock);
        return;
    }

    const uint32_t pb_len = static_cast<uint32_t>(pb_head.size());
    char len_buf[4];
    memcpy(len_buf, &pb_len, 4);

    send(sock, len_buf, 4, 0);
    send(sock, pb_head.data(), pb_head.size(), 0);

    // 2. 接收 presigned url
    char url_buf[2048] = {0};
    const int n = recv(sock, url_buf, sizeof(url_buf) - 1, 0);
    ::close(sock);

    if (n <= 0)
    {
        logEdit->append("未收到外链！");
        return;
    }

    const QString presignedUrl = QString::fromUtf8(url_buf);

    // 3. 用浏览器打开外链
    QDesktopServices::openUrl(QUrl(presignedUrl));
}