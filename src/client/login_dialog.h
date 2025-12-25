#pragma once

#include <QDialog>

class QLineEdit;
class QPushButton;
class QLabel;

class LoginDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit LoginDialog(QWidget* parent = nullptr);

    QString token() const { return token_; }

private slots:
    void onLoginClicked();
    void onRegisterClicked();

private:
    struct Result {
        bool ok = false;
        QString token;
        QString msg;
    };

    Result doAuth_(int type /*11 login, 12 register*/, const QString& user, const QString& pass);

private:
    QLineEdit* userEdit_ = nullptr;
    QLineEdit* passEdit_ = nullptr;
    QPushButton* loginBtn_ = nullptr;
    QPushButton* regBtn_ = nullptr;
    QLabel* tipLabel_ = nullptr;

    QString token_;
};