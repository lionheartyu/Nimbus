#pragma once

#include <QDialog>

class QLineEdit;
class QPushButton;
class QLabel;

/// Nimbus 登录对话框
class LoginDialog final : public QDialog
{
    Q_OBJECT
public:
    /// 构造函数
    explicit LoginDialog(QWidget* parent = nullptr);

    /// 获取登录成功后的 token
    QString token() const { return token_; }

private slots:
    /// 登录按钮点击槽函数
    void onLoginClicked();

    /// 注册按钮点击槽函数
    void onRegisterClicked();

private:
    /// 登录/注册请求结果结构体
    struct Result {
        bool ok = false;      ///< 是否成功
        QString token;        ///< 登录成功返回的 token
        QString msg;          ///< 错误或提示信息
    };

    /// 执行登录/注册请求
    /// @param type 11=登录，12=注册
    /// @param user 用户名
    /// @param pass 密码
    Result doAuth_(int type, const QString& user, const QString& pass);

private:
    QLineEdit* userEdit_ = nullptr;    ///< 用户名输入框
    QLineEdit* passEdit_ = nullptr;    ///< 密码输入框
    QPushButton* loginBtn_ = nullptr;  ///< 登录按钮
    QPushButton* regBtn_ = nullptr;    ///< 注册按钮
    QLabel* tipLabel_ = nullptr;       ///< 提示信息标签

    QString token_;                    ///< 登录成功后的 token
};