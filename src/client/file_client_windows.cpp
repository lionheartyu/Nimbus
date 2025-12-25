#include "file_client_windows.h"
#include "draggable_listwidget.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QScrollArea>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QMediaPlayer>   
#include <QVideoWidget>   
#include "../../proto/file.pb.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <QUuid> // add

namespace {

// 避免 UI 卡死：给 socket 设置读写超时（秒）
// 原来是 10 秒，5GB+ 上传必然触发 SO_SNDTIMEO 超时 -> sendAll 失败
constexpr int kSockTimeoutSecShort    = 10;
constexpr int kSockTimeoutSecTransfer = 2 * 60 * 60; // 2小时（按你网络情况可再调大）

// 防止服务端/网络异常导致分配超大内存或一直等
constexpr uint32_t kMaxListResponseBytes = 8u * 1024u * 1024u;     // 8MB 足够装文件名列表
constexpr uint32_t kMaxDownloadBytes     = 1024u * 1024u * 1024u;  // 1GB（按需调整）

constexpr qint64 kChunkThreshold = 5ll * 1024 * 1024 * 1024; // 5GB
constexpr qint64 kChunkSize      = 64ll * 1024 * 1024;       // 64MB

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

static void drainSocket_(int sock)
{
    // 尽量把服务端已经开始发的内容读掉，避免服务端继续 send 导致 EPIPE/刷日志
    std::vector<char> tmp(64 * 1024);
    for (;;)
    {
        const ssize_t n = ::recv(sock, tmp.data(), tmp.size(), 0);
        if (n > 0) continue;
        break; // 0=对端关闭；-1=超时/错误
    }
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

bool connectToServer(int& sock, const char* ip, uint16_t port, int timeoutSec)
{
    sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    setSocketTimeouts(sock, timeoutSec);

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

// 保留原函数：默认短超时（列表/删除/外链/预览等）
bool connectToServer(int& sock, const char* ip, uint16_t port)
{
    return connectToServer(sock, ip, port, kSockTimeoutSecShort);
}

bool sendPbHeader(int sock, const std::string& pb)
{
    uint32_t pb_len = static_cast<uint32_t>(pb.size());
    // 注意：你服务端目前显然按“小端/原样 uint32”读，这里保持一致（不做 htonl）
    return sendAll(sock, &pb_len, sizeof(pb_len)) && sendAll(sock, pb.data(), pb.size());
}

static inline QString fileExtLower_(const QString& name)
{
    const int dot = name.lastIndexOf('.');
    if (dot < 0) return QString();
    return name.mid(dot + 1).toLower();
}

static inline bool isImageExt_(const QString& ext)
{
    static const QStringList exts = {"png","jpg","jpeg","bmp","gif","webp"};
    return exts.contains(ext);
}

static inline bool isTextExt_(const QString& ext)
{
    static const QStringList exts = {"txt","log","md","json","xml","yaml","yml","ini","cfg","conf","csv","h","hpp","c","cc","cpp","py","js","ts","java","go","rs","sh"};
    return exts.contains(ext);
}

static inline bool isVideoExt_(const QString& ext)
{
    static const QStringList exts = {"mp4","mkv","webm","flv","avi","mov","wmv","mpg","mpeg"};
    return exts.contains(ext);
}

static inline QString recvErrorTextAfterLen0_(int sock)
{
    // 最多读 64KB 错误文本
    std::string out;
    out.reserve(512);
    char buf[4096];

    size_t total = 0;
    while (total < 64 * 1024)
    {
        const ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
        if (n > 0)
        {
            out.append(buf, buf + n);
            total += static_cast<size_t>(n);
            continue;
        }
        break;
    }

    QString q = QString::fromUtf8(out.data(), static_cast<int>(out.size())).trimmed();
    return q.isEmpty() ? QString("ERROR: 未知错误") : q;
}

// 读取 [uint32 len][payload]；len=0 时把后面的 ERROR 文本读出来
static bool recvLenAndPayload_(int sock, uint32_t maxLen, QByteArray* out, QString* err)
{
    uint32_t len = 0;
    if (!recvAll(sock, &len, sizeof(len)))
    {
        if (err) *err = "未收到服务器响应长度（可能超时）！";
        return false;
    }

    if (len == 0)
    {
        if (err) *err = recvErrorTextAfterLen0_(sock);
        return false;
    }

    if (len > maxLen)
    {
        if (err) *err = "预览内容过大，已拒绝加载。";
        return false;
    }

    out->resize(static_cast<int>(len));
    if (len > 0 && !recvAll(sock, out->data(), len))
    {
        if (err) *err = "未收到完整预览数据（可能超时/断开）！";
        return false;
    }
    return true;
}

template <typename Fn, typename Done>
static void runBusyWithProgress_(QWidget* parent,
                                const QString& title,
                                const QString& text,
                                Fn&& worker,
                                Done&& onDone)
{
    auto* dlg = new QProgressDialog(text, QString(), 0, 0, parent);
    dlg->setWindowTitle(title);
    dlg->setWindowModality(Qt::ApplicationModal);
    dlg->setCancelButton(nullptr);
    dlg->setMinimumDuration(0);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    dlg->show();

    using R = decltype(worker());
    auto* watcher = new QFutureWatcher<R>(parent);

    QObject::connect(watcher, &QFutureWatcher<R>::finished, parent, [watcher, dlg, onDone = std::forward<Done>(onDone)]() mutable {
        dlg->close();
        dlg->deleteLater();

        const R r = watcher->result();
        watcher->deleteLater();

        onDone(r);
    });

    watcher->setFuture(QtConcurrent::run(std::forward<Fn>(worker)));
}

static inline QString makeUploadId_()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

static inline std::string tokenExtra_(const QString& token)
{
    if (token.trimmed().isEmpty()) return std::string();
    return "token=" + token.toStdString();
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
    QLabel* title = new QLabel("Nimbus 云盘在线客户端系统", this);
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

    QFileInfo info(filePath);
    const qint64 filesize = file.size();
    const QString relPath = info.fileName();

    // ===== 5GB+ 分片上传（选A：最后一片触发合并并上传）=====
    if (filesize >= kChunkThreshold)
    {
        const QString uploadId = makeUploadId_();
        const qint64 total = (filesize + kChunkSize - 1) / kChunkSize;

        logEdit->append(QString("大文件分片上传: %1, size=%2, chunks=%3, uploadId=%4")
                            .arg(relPath)
                            .arg(filesize)
                            .arg(total)
                            .arg(uploadId));

        for (qint64 idx = 0; idx < total; ++idx)
        {
            const qint64 offset = idx * kChunkSize;
            const qint64 thisSize = std::min(kChunkSize, filesize - offset);

            if (!file.seek(offset))
            {
                logEdit->append("分片 seek 失败");
                return;
            }

            // 注意：这里为了兼容服务端“最小实现（要求一片一次性到齐）”，我们一次 read 一片到内存并一次 sendAll 发完。
            // 64MB 一片通常可接受；如果你机器内存紧张，可把服务端改成“保存分片接收状态”后再做流式读写。
            QByteArray chunk = file.read(thisSize);
            if (chunk.size() != thisSize)
            {
                logEdit->append("分片读取失败");
                return;
            }

            FileHeader header;
            header.set_type(10);
            header.set_filename(relPath.toStdString());
            header.set_filesize(static_cast<uint64_t>(thisSize));

            // extra: token=...;uploadId=...;index=...;...
            const std::string base = tokenExtra_(token_);
            const std::string extra =
                (base.empty() ? "" : (base + ";")) +
                "uploadId=" + uploadId.toStdString() +
                ";index=" + std::to_string(idx) +
                ";total=" + std::to_string(total) +
                ";full=" + std::to_string(static_cast<uint64_t>(filesize)) +
                ";chunk=" + std::to_string(static_cast<uint64_t>(thisSize));

            header.set_extra(extra);

            std::string pb_head;
            header.SerializeToString(&pb_head);

            int sock = -1;
            if (!connectToServer(sock, "10.20.32.88", 8080, kSockTimeoutSecTransfer))
            {
                logEdit->append("连接服务器失败（分片）");
                return;
            }

            if (!sendPbHeader(sock, pb_head))
            {
                logEdit->append("发送分片头失败");
                ::close(sock);
                return;
            }

            if (!sendAll(sock, chunk.data(), static_cast<size_t>(chunk.size())))
            {
                logEdit->append("发送分片数据失败");
                ::close(sock);
                return;
            }

            char resp[256] = {0};
            const int rn = ::recv(sock, resp, sizeof(resp) - 1, 0);
            ::close(sock);

            const QString respText = (rn > 0) ? QString::fromUtf8(resp, rn).trimmed() : QString();
            if (idx + 1 < total)
            {
                if (!respText.startsWith("CHUNK OK"))
                {
                    logEdit->append("分片上传失败: " + respText);
                    return;
                }
            }
            else
            {
                // 最后一片：服务端会合并并上传，返回 UPLOAD OK（或 ERROR）
                if (!respText.startsWith("UPLOAD OK"))
                {
                    logEdit->append("合并/上传失败: " + respText);
                    return;
                }
            }

            const int percent = static_cast<int>(((idx + 1) * 100) / total);
            progressBar->setValue(percent);
            QCoreApplication::processEvents();
        }

        QMessageBox::information(this, "上传成功", "大文件分片上传完成：\n" + relPath);
        progressBar->setValue(100);
        if (!inRecycle) QTimer::singleShot(200, this, &FileClientWindow::onList);
        return;
    }

    // ===== 小文件：走你原来的直传逻辑（加 token）=====
    FileHeader header;
    header.set_filename(relPath.toStdString());
    header.set_filesize(filesize);
    header.set_extra(tokenExtra_(token_)); // add

    std::string pb_head;
    header.SerializeToString(&pb_head);

    int sock = -1;
    // 关键：上传用长超时（否则 5GB+ 极易触发 SO_SNDTIMEO 超时）
    if (!connectToServer(sock, "10.20.32.88", 8080, kSockTimeoutSecTransfer))
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
    header.set_extra(tokenExtra_(token_)); // add

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
    header.set_extra(tokenExtra_(token_)); // add

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
        logEdit->append("文件过大，已取消下载（不会影响服务端）。");

        // 先把连接处理掉：不要等用户点 OK
        // 先半关闭读端（可选，但能更快触发对端感知）
        ::shutdown(sock, SHUT_RD);
        drainSocket_(sock);
        ::close(sock);

        progressBar->setValue(0);

        // 再弹窗（此时不会再拖累服务端）
        QMessageBox msg(this);
        msg.setIcon(QMessageBox::Information);
        msg.setWindowTitle("文件过大，已取消下载");
        msg.setText("该文件超过下载大小限制，已拒绝下载。");
        msg.setInformativeText("你可以在文件列表里右键选择“查看文件”，或直接双击文件进行查看。");
        msg.setStandardButtons(QMessageBox::Ok);
        msg.exec();

        // 自动“重连继续”
        QTimer::singleShot(100, this, [this]() {
            if (!inRecycle) onList();
            else onRecycle();
        });
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
    const QString filenameShown = item->text();

    // 关键修复：在回收站模式下，请求里的 filename 必须带 recycle/ 前缀，
    // 否则服务端会找错对象（可能导致“还原后名字变了/变成重复文件名”等问题）
    const QString filenameForReq = inRecycle ? ("recycle/" + filenameShown) : filenameShown;

    if (selectedAction == downloadAction)
    {
        filePathEdit->setText(filenameForReq);
        onDownload();
        return;
    }

    if (selectedAction == viewAction)
    {
        // 双击查看里会自己处理 recycle/ 前缀，你这里也直接走双击逻辑即可
        onFileDoubleClicked(item);
        return;
    }

    // ===== 删除/还原/彻底删除：统一走后台 + 进度条 =====
    auto runOpWithProgress_ = [this](const QString& title,
                                    const QString& text,
                                    int type,
                                    const QString& filenameForReqInner,
                                    std::function<void(const QString& resp)> onSuccess) {
        auto* dlg = new QProgressDialog(text, QString(), 0, 0, this);
        dlg->setWindowTitle(title);
        dlg->setWindowModality(Qt::ApplicationModal);
        dlg->setCancelButton(nullptr);
        dlg->setMinimumDuration(0);
        dlg->setAutoClose(false);
        dlg->setAutoReset(false);
        dlg->show();

        struct Result {
            bool ok = false;
            QString resp;
            QString err;
        };

        auto* watcher = new QFutureWatcher<Result>(this);
        const QString nameCopy = filenameForReqInner;

        auto future = QtConcurrent::run([nameCopy, type, tokenCopy = token_]() -> Result {
            Result r;

            FileHeader header;
            header.set_filename(nameCopy.toStdString());
            header.set_type(type);
            header.set_extra(tokenExtra_(tokenCopy)); // add

            std::string pb_head;
            header.SerializeToString(&pb_head);

            int sock = -1;
            if (!connectToServer(sock, "10.20.32.88", 8080))
            {
                r.err = "连接服务器失败！";
                return r;
            }

            if (!sendPbHeader(sock, pb_head))
            {
                r.err = "发送请求失败！";
                ::close(sock);
                return r;
            }

            char respBuf[256] = {0};
            const int n = ::recv(sock, respBuf, sizeof(respBuf) - 1, 0);
            ::close(sock);

            if (n <= 0)
            {
                r.err = "未收到服务器响应（可能超时/断开）！";
                return r;
            }

            r.ok = true;
            r.resp = QString::fromUtf8(respBuf, n).trimmed();
            return r;
        });

        connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, dlg, type, onSuccess]() {
            dlg->close();
            dlg->deleteLater();

            const Result r = watcher->result();
            watcher->deleteLater();

            if (!r.ok)
            {
                logEdit->append(r.err);
                return;
            }

            logEdit->append("服务器响应: " + r.resp);

            // 关键：只在明确成功时才算成功，才弹窗/刷新
            const QString up = r.resp.toUpper();
            bool success = false;
            if (type == 4) success = up.startsWith("DELETE OK");
            if (type == 6) success = up.startsWith("RESTORE OK");
            if (type == 7) success = up.startsWith("REMOVE OK");

            if (!success)
            {
                // 失败/未知响应：不弹“已提交…”
                return;
            }

            onSuccess(r.resp);
        });

        watcher->setFuture(future);
    };

    if (selectedAction == deleteAction)
    {
        runOpWithProgress_(
            "删除文件",
            "正在删除并移入回收站…",
            4,
            filenameForReq,
            [this, filenameShown](const QString&) {
                QMessageBox::information(this, "删除完成", "删除成功：\n" + filenameShown);
                onList();
            });
        return;
    }

    if (selectedAction == restoreAction)
    {
        runOpWithProgress_(
            "还原文件",
            "正在还原…",
            6,
            filenameForReq, // fix: 使用带 recycle/ 前缀的路径
            [this, filenameShown](const QString&) {
                QMessageBox::information(this, "还原完成", "还原成功：\n" + filenameShown);
                onRecycle();
            });
        return;
    }

    if (selectedAction == removeAction)
    {
        runOpWithProgress_(
            "彻底删除",
            "正在彻底删除…",
            7,
            filenameForReq, // fix: 使用带 recycle/ 前缀的路径
            [this, filenameShown](const QString&) {
                QMessageBox::information(this, "删除完成", "已彻底删除：\n" + filenameShown);
                onRecycle();
            });
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

    int sock = -1;
    // 关键：目录/拖拽上传也要长超时
    if (!connectToServer(sock, "10.20.32.88", 8080, kSockTimeoutSecTransfer))
    {
        logEdit->append("连接服务器失败！");
        return;
    }

    // 关键：统一用 sendPbHeader + sendAll，避免短写
    if (!sendPbHeader(sock, pb_head))
    {
        logEdit->append("发送上传头失败（可能超时/断开）！");
        ::close(sock);
        return;
    }

    std::vector<char> buf(64 * 1024);
    qint64 sent = 0;

    while (!file.atEnd())
    {
        const qint64 n = file.read(buf.data(), static_cast<qint64>(buf.size()));
        if (n > 0)
        {
            if (!sendAll(sock, buf.data(), static_cast<size_t>(n)))
            {
                logEdit->append("上传中断（可能超时/断开）: " + relPath);
                ::close(sock);
                return;
            }
            sent += n;

            const int percent = (filesize > 0) ? static_cast<int>((sent * 100) / filesize) : 100;
            progressBar->setValue(percent);
            QCoreApplication::processEvents();
        }
    }
    file.close();

    char resp[128] = {0};
    const int n = ::recv(sock, resp, sizeof(resp) - 1, 0);
    ::close(sock);

    if (n > 0)
        logEdit->append("上传: " + relPath + " -> " + QString::fromUtf8(resp, n).trimmed());
    else
        logEdit->append("上传: " + relPath + " 未收到服务器响应！");
}

// 新增：回收站按钮槽函数，实现回收站文件列举逻辑
void FileClientWindow::onRecycle()
{
    FileHeader header;
    header.set_type(5);
    header.set_extra(tokenExtra_(token_)); // add

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
            // 修复：列表里显示的名字要保持服务端“完整相对路径”，不要只取最后一段
            // 原逻辑可能把路径/文件名处理错，导致还原/删除后看起来“文件名变了”
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

    const QString ext = fileExtLower_(filename);

    // ===== 1) 图片/文本：走 type=9 内置预览 =====
    if (isImageExt_(ext) || isTextExt_(ext))
    {
        // 小进度条（无限忙碌）
        auto* dlg = new QProgressDialog("正在加载预览…", QString(), 0, 0, this);
        dlg->setWindowTitle("查看文件");
        dlg->setWindowModality(Qt::ApplicationModal);
        dlg->setCancelButton(nullptr);
        dlg->setMinimumDuration(0);
        dlg->setAutoClose(false);
        dlg->setAutoReset(false);
        dlg->show();

        struct Result {
            bool ok = false;
            QByteArray payload;
            QString err;
        };

        auto* watcher = new QFutureWatcher<Result>(this);

        const QString filenameCopy = filename;
        const QString extCopy = ext;

        // 预览 type=9（原本 extra 放 max bytes，改成 token + max）
        auto future = QtConcurrent::run([filenameCopy, tokenCopy = token_]() -> Result {
            Result r;

            FileHeader header;
            header.set_filename(filenameCopy.toStdString());
            header.set_type(9);

            const std::string base = tokenExtra_(tokenCopy);
            header.set_extra((base.empty() ? "" : (base + ";")) + std::string("max=2097152"));

            std::string pb_head;
            header.SerializeToString(&pb_head);

            int sock = -1;
            if (!connectToServer(sock, "10.20.32.88", 8080))
            {
                r.err = "连接服务器失败！";
                return r;
            }

            if (!sendPbHeader(sock, pb_head))
            {
                r.err = "发送预览请求失败！";
                ::close(sock);
                return r;
            }

            QString err;
            QByteArray payload;
            const uint32_t kMaxPreviewBytes = 2u * 1024u * 1024u;
            if (!recvLenAndPayload_(sock, kMaxPreviewBytes, &payload, &err))
            {
                r.err = err;
                ::close(sock);
                return r;
            }

            ::close(sock);
            r.ok = true;
            r.payload = payload;
            return r;
        });

        connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, dlg, extCopy, filenameCopy]() {
            dlg->close();
            dlg->deleteLater();

            const Result r = watcher->result();
            watcher->deleteLater();

            if (!r.ok)
            {
                logEdit->append("预览失败: " + r.err);
                return;
            }

            // ====== 保留你原来的“图片/文本弹窗显示”逻辑 ======
            if (isImageExt_(extCopy))
            {
                QBuffer buffer;
                buffer.setData(r.payload);
                if (!buffer.open(QIODevice::ReadOnly))
                {
                    logEdit->append("图片预览失败：无法打开内存缓冲区。");
                    return;
                }

                QImageReader reader(&buffer);
                reader.setDecideFormatFromContent(true);

                const QImage img = reader.read();
                if (img.isNull())
                {
                    logEdit->append("图片预览失败：无法解码图片内容。");
                    return;
                }

                const QPixmap pix = QPixmap::fromImage(img);

                auto* dlg2 = new QDialog(this);
                dlg2->setAttribute(Qt::WA_DeleteOnClose, true);
                dlg2->setWindowTitle("预览图片 - " + filenameCopy);
                dlg2->resize(900, 700);

                auto* label = new QLabel(dlg2);
                label->setPixmap(pix);
                label->setAlignment(Qt::AlignCenter);

                auto* scroll = new QScrollArea(dlg2);
                scroll->setWidget(label);
                scroll->setWidgetResizable(true);

                auto* layout = new QVBoxLayout(dlg2);
                layout->addWidget(scroll);
                dlg2->setLayout(layout);
                dlg2->show();
                return;
            }

            if (isTextExt_(extCopy))
            {
                auto* dlg2 = new QDialog(this);
                dlg2->setAttribute(Qt::WA_DeleteOnClose, true);
                dlg2->setWindowTitle("预览文本 - " + filenameCopy);
                dlg2->resize(900, 700);

                auto* edit = new QPlainTextEdit(dlg2);
                edit->setReadOnly(true);
                edit->setPlainText(QString::fromUtf8(r.payload));
                edit->setLineWrapMode(QPlainTextEdit::NoWrap);

                auto* layout = new QVBoxLayout(dlg2);
                layout->addWidget(edit);
                dlg2->setLayout(layout);
                dlg2->show();
                return;
            }
        });

        watcher->setFuture(future);
        return;
    }

    // ===== 视频：走 type=8 拿到外链，然后用 Qt 内置播放器在线播放 =====
    if (isVideoExt_(ext))
    {
        auto* dlg = new QProgressDialog("正在生成视频播放链接…", QString(), 0, 0, this);
        dlg->setWindowTitle("视频预览");
        dlg->setWindowModality(Qt::ApplicationModal);
        dlg->setCancelButton(nullptr);
        dlg->setMinimumDuration(0);
        dlg->setAutoClose(false);
        dlg->setAutoReset(false);
        dlg->show();

        struct Result {
            bool ok = false;
            QString url;
            QString err;
        };

        const QString filenameCopy = filename;
        auto* watcher = new QFutureWatcher<Result>(this);

        // 外链 type=8
        auto future = QtConcurrent::run([filenameCopy, tokenCopy = token_]() -> Result {
            Result r;

            FileHeader header;
            header.set_filename(filenameCopy.toStdString());
            header.set_type(8);
            header.set_extra(tokenExtra_(tokenCopy)); // fix: 其它类型也要带 token

            std::string pb_head;
            header.SerializeToString(&pb_head);

            int sock = -1;
            if (!connectToServer(sock, "10.20.32.88", 8080))
            {
                r.err = "连接服务器失败！";
                return r;
            }

            if (!sendPbHeader(sock, pb_head))
            {
                r.err = "发送外链请求失败！";
                ::close(sock);
                return r;
            }

            std::string url;
            url.reserve(2048);
            char b[1024];
            for (;;)
            {
                const ssize_t n = ::recv(sock, b, sizeof(b), 0);
                if (n > 0) { url.append(b, b + n); continue; }
                break;
            }
            ::close(sock);

            const QString presignedUrl = QString::fromUtf8(url.data(), static_cast<int>(url.size())).trimmed();
            if (presignedUrl.isEmpty())
            {
                r.err = "未收到外链！";
                return r;
            }
            if (presignedUrl.startsWith("ERROR", Qt::CaseInsensitive))
            {
                r.err = presignedUrl;
                return r;
            }

            r.ok = true;
            r.url = presignedUrl;
            return r;
        });

        connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, dlg, filenameCopy]() {
            dlg->close();
            dlg->deleteLater();

            const Result r = watcher->result();
            watcher->deleteLater();

            if (!r.ok)
            {
                logEdit->append("视频预览失败: " + r.err);
                return;
            }

            // Qt 视频播放窗口（独立关闭不影响主窗口）
            auto* w = new QDialog(nullptr); // 不以 main window 作为 parent，避免级联销毁/关闭逻辑
            w->setAttribute(Qt::WA_DeleteOnClose, true);
            w->setAttribute(Qt::WA_QuitOnClose, false); // 关键：关闭这个窗口不要退出整个应用
            w->setWindowTitle("视频预览 - " + filenameCopy);
            w->resize(980, 640);

            auto* video = new QVideoWidget(w);
            video->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            auto* player = new QMediaPlayer(nullptr);
            auto* audio  = new QAudioOutput(nullptr);

            player->setAudioOutput(audio);
            player->setVideoOutput(video);
            player->setSource(QUrl(r.url));
            audio->setVolume(1.0);

            // 关键：在窗口关闭(accept/reject)时先 stop + 断开 video output，再 deleteLater
            QObject::connect(w, &QDialog::finished, w, [player, audio]() {
                player->stop();
                player->setVideoOutput(nullptr);
                player->deleteLater();
                audio->deleteLater();
            });
#else
            auto* player = new QMediaPlayer(nullptr);
            player->setVideoOutput(video);
            player->setMedia(QMediaContent(QUrl(r.url)));

            QObject::connect(w, &QDialog::finished, w, [player]() {
                player->stop();
                player->deleteLater();
            });
#endif

            auto* layout = new QVBoxLayout(w);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->addWidget(video);
            w->setLayout(layout);

            w->show();
            player->play();
        });

        watcher->setFuture(future);
        return;
    }

    // ===== 其它类型：保持你原来的 type=8 外链用系统打开 =====
    {
        auto* dlg = new QProgressDialog("正在生成查看链接…", QString(), 0, 0, this);
        dlg->setWindowTitle("查看文件");
        dlg->setWindowModality(Qt::ApplicationModal);
        dlg->setCancelButton(nullptr);
        dlg->setMinimumDuration(0);
        dlg->setAutoClose(false);
        dlg->setAutoReset(false);
        dlg->show();

        struct Result {
            bool ok = false;
            QString url;
            QString err;
        };

        auto* watcher = new QFutureWatcher<Result>(this);

        const QString filenameCopy = filename;

        // fix: 这里之前没带 token，导致 ERROR: Unauthorized
        auto future = QtConcurrent::run([filenameCopy, tokenCopy = token_]() -> Result {
            Result r;

            FileHeader header;
            header.set_filename(filenameCopy.toStdString());
            header.set_type(8);
            header.set_extra(tokenExtra_(tokenCopy)); // add token

            std::string pb_head;
            header.SerializeToString(&pb_head);

            int sock = -1;
            if (!connectToServer(sock, "10.20.32.88", 8080))
            {
                r.err = "连接服务器失败！";
                return r;
            }

            if (!sendPbHeader(sock, pb_head))
            {
                r.err = "发送外链请求失败！";
                ::close(sock);
                return r;
            }

            std::string url;
            url.reserve(2048);
            char b[1024];
            for (;;)
            {
                const ssize_t n = ::recv(sock, b, sizeof(b), 0);
                if (n > 0) { url.append(b, b + n); continue; }
                break;
            }
            ::close(sock);

            const QString presignedUrl = QString::fromUtf8(url.data(), static_cast<int>(url.size())).trimmed();
            if (presignedUrl.isEmpty())
            {
                r.err = "未收到外链！";
                return r;
            }
            if (presignedUrl.startsWith("ERROR", Qt::CaseInsensitive))
            {
                r.err = presignedUrl;
                return r;
            }

            r.ok = true;
            r.url = presignedUrl;
            return r;
        });

        connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, dlg]() {
            dlg->close();
            dlg->deleteLater();

            const Result r = watcher->result();
            watcher->deleteLater();

            if (!r.ok)
            {
                logEdit->append("查看失败: " + r.err);
                return;
            }

            QDesktopServices::openUrl(QUrl(r.url));
        });

        watcher->setFuture(future);
    }
}