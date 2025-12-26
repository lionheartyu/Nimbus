#pragma once

#include <QDialog>

class QLineEdit;
class QPushButton;
class QLabel;

/// Nimbus 注册对话框
class RegisterDialog final : public QDialog
{
    Q_OBJECT
public:
    /// 构造函数
    explicit RegisterDialog(QWidget* parent = nullptr);

private slots:
    /// 注册按钮点击槽函数
    void onRegister();

private:
    /// 注册请求结果结构体
    struct Result {
        bool ok = false;   ///< 是否注册成功
        QString msg;       ///< 错误或提示信息
    };

    /// 执行注册请求
    /// @param user 用户名
    /// @param pass 密码
    Result doRegister_(const QString& user, const QString& pass);

private:
    QLineEdit* userEdit_ = nullptr;    ///< 用户名输入框
    QLineEdit* passEdit_ = nullptr;    ///< 密码输入框
    QLineEdit* pass2Edit_ = nullptr;   ///< 确认密码输入框
    QPushButton* regBtn_ = nullptr;    ///< 注册按钮
    QLabel* tipLabel_ = nullptr;       ///< 提示信息标签
};