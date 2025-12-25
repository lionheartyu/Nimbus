#include "login_dialog.h"
#include "register_dialog.h"

#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QGraphicsDropShadowEffect>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

#include "../../proto/file.pb.h"

namespace {

constexpr int kSockTimeoutSecShort = 10;

bool setSocketTimeouts(int sock, int seconds)
{
    timeval tv{};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return false;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return false;
    return true;
}

bool sendAll(int sock, const void* data, size_t len)
{
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len)
    {
        const ssize_t n = ::send(sock, p + sent, len - sent, 0);
        if (n > 0) { sent += static_cast<size_t>(n); continue; }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

bool connectToServer(int& sock, const char* ip, uint16_t port)
{
    sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    setSocketTimeouts(sock, kSockTimeoutSecShort);

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
    const uint32_t pb_len = static_cast<uint32_t>(pb.size());
    return sendAll(sock, &pb_len, sizeof(pb_len)) && sendAll(sock, pb.data(), pb.size());
}

static inline QString readAllText_(int sock)
{
    std::string out;
    out.reserve(512);
    char buf[1024];
    for (;;)
    {
        const ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
        if (n > 0) { out.append(buf, buf + n); continue; }
        break;
    }
    return QString::fromUtf8(out.data(), static_cast<int>(out.size())).trimmed();
}

static inline QString parseToken_(const QString& resp)
{
    const int p = resp.indexOf("token=");
    if (p < 0) return {};
    return resp.mid(p + 6).trimmed();
}

static inline QString authStyleSheet_()
{
    return QString(
        "QDialog{ background:#f6f7fb; }"
        "QLabel#Title{ font-size:18px; font-weight:700; color:#111827; }"
        "QLabel#SubTitle{ color:#6b7280; }"
        "QLineEdit{ background:#ffffff; border:1px solid #e5e7eb; border-radius:10px; padding:10px 12px; font-size:14px; color:#111827; }"
        "QLineEdit:focus{ border:1px solid #2d77ff; }"
        "QPushButton#Primary{ background:#2d77ff; color:#ffffff; border:none; border-radius:10px; padding:10px 14px; font-weight:700; }"
        "QPushButton#Primary:hover{ background:#2563eb; }"
        "QPushButton#Primary:pressed{ background:#1d4ed8; }"
        "QPushButton#Ghost{ background:transparent; color:#2d77ff; border:none; padding:10px 10px; font-weight:700; }"
        "QPushButton#Ghost:hover{ text-decoration: underline; }"
        "QLabel#Tip{ color:#6b7280; }"
        "QWidget#Card{ background:#ffffff; border:1px solid #eef0f4; border-radius:14px; }"
    );
}

static inline QWidget* makeCard_(QWidget* parent)
{
    auto* card = new QWidget(parent);
    card->setObjectName("Card");

    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(18);
    shadow->setOffset(0, 10);
    shadow->setColor(QColor(17, 24, 39, 28));
    card->setGraphicsEffect(shadow);
    return card;
}

} // namespace

LoginDialog::LoginDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Nimbus 登录");
    setModal(true);
    setFixedSize(460, 320);
    setStyleSheet(authStyleSheet_());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 18);

    QWidget* card = makeCard_(this);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(22, 20, 22, 18);
    cardLayout->setSpacing(10);

    auto* title = new QLabel("欢迎使用 Nimbus", card);
    title->setObjectName("Title");

    auto* sub = new QLabel("登录后即可使用云盘上传、下载与预览。", card);
    sub->setObjectName("SubTitle");
    sub->setWordWrap(true);

    userEdit_ = new QLineEdit(card);
    userEdit_->setPlaceholderText("用户名");
    userEdit_->setMinimumHeight(40);

    passEdit_ = new QLineEdit(card);
    passEdit_->setPlaceholderText("密码");
    passEdit_->setEchoMode(QLineEdit::Password);
    passEdit_->setMinimumHeight(40);

    tipLabel_ = new QLabel("请输入账号密码登录，或点击注册创建新账号。", card);
    tipLabel_->setObjectName("Tip");
    tipLabel_->setWordWrap(true);

    loginBtn_ = new QPushButton("登录", card);
    loginBtn_->setObjectName("Primary");
    loginBtn_->setMinimumHeight(42);
    loginBtn_->setDefault(true);

    regBtn_ = new QPushButton("注册账号", card);
    regBtn_->setObjectName("Ghost");
    regBtn_->setMinimumHeight(42);

    auto* btnRow = new QHBoxLayout;
    btnRow->addWidget(regBtn_);
    btnRow->addStretch(1);
    btnRow->addWidget(loginBtn_);

    cardLayout->addWidget(title);
    cardLayout->addWidget(sub);
    cardLayout->addSpacing(6);
    cardLayout->addWidget(userEdit_);
    cardLayout->addWidget(passEdit_);
    cardLayout->addWidget(tipLabel_);
    cardLayout->addSpacing(4);
    cardLayout->addLayout(btnRow);

    root->addWidget(card);

    connect(loginBtn_, &QPushButton::clicked, this, &LoginDialog::onLoginClicked);
    connect(regBtn_, &QPushButton::clicked, this, &LoginDialog::onRegisterClicked);
}

LoginDialog::Result LoginDialog::doAuth_(int type, const QString& user, const QString& pass)
{
    Result r;

    FileHeader header;
    header.set_type(type);
    header.set_filename(user.toStdString());
    header.set_extra(pass.toStdString());

    std::string pb;
    header.SerializeToString(&pb);

    int sock = -1;
    if (!connectToServer(sock, "10.20.32.88", 8080))
    {
        r.msg = "连接服务器失败";
        return r;
    }

    if (!sendPbHeader(sock, pb))
    {
        ::close(sock);
        r.msg = "发送请求失败";
        return r;
    }

    const QString resp = readAllText_(sock);
    ::close(sock);

    if (resp.startsWith("ERROR", Qt::CaseInsensitive) || resp.isEmpty())
    {
        r.msg = resp.isEmpty() ? "未收到服务器响应" : resp;
        return r;
    }

    if (type == 12)
    {
        r.ok = resp.startsWith("REGISTER OK", Qt::CaseInsensitive);
        r.msg = resp;
        return r;
    }

    if (!resp.startsWith("LOGIN OK", Qt::CaseInsensitive))
    {
        r.msg = resp;
        return r;
    }

    const QString tk = parseToken_(resp);
    if (tk.isEmpty())
    {
        r.msg = "登录响应缺少 token";
        return r;
    }

    r.ok = true;
    r.token = tk;
    r.msg = resp;
    return r;
}

void LoginDialog::onLoginClicked()
{
    const QString user = userEdit_->text().trimmed();
    const QString pass = passEdit_->text();

    if (user.isEmpty() || pass.isEmpty())
    {
        QMessageBox::warning(this, "提示", "请输入用户名和密码");
        return;
    }

    // 不阻塞 UI：禁用控件 + 后台线程登录
    loginBtn_->setEnabled(false);
    regBtn_->setEnabled(false);
    userEdit_->setEnabled(false);
    passEdit_->setEnabled(false);
    tipLabel_->setText("正在登录…");

    struct R { bool ok; QString token; QString msg; };
    auto* watcher = new QFutureWatcher<R>(this);

    watcher->setFuture(QtConcurrent::run([this, user, pass]() -> R {
        const Result rr = doAuth_(11, user, pass);
        return R{rr.ok, rr.token, rr.msg};
    }));

    connect(watcher, &QFutureWatcher<R>::finished, this, [this, watcher]() {
        const R r = watcher->result();
        watcher->deleteLater();

        loginBtn_->setEnabled(true);
        regBtn_->setEnabled(true);
        userEdit_->setEnabled(true);
        passEdit_->setEnabled(true);

        if (!r.ok)
        {
            tipLabel_->setText("登录失败，请检查用户名/密码或服务器状态。");
            QMessageBox::warning(this, "登录失败", r.msg);
            return;
        }

        token_ = r.token;
        tipLabel_->setText("登录成功。");
        accept();
    });
}

void LoginDialog::onRegisterClicked()
{
    RegisterDialog reg(this);
    reg.exec();
}