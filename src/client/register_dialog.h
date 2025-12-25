#pragma once

#include <QDialog>

class QLineEdit;
class QPushButton;
class QLabel;

class RegisterDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit RegisterDialog(QWidget* parent = nullptr);

private slots:
    void onRegister();

private:
    struct Result {
        bool ok = false;
        QString msg;
    };

    Result doRegister_(const QString& user, const QString& pass);

private:
    QLineEdit* userEdit_ = nullptr;
    QLineEdit* passEdit_ = nullptr;
    QLineEdit* pass2Edit_ = nullptr;
    QPushButton* regBtn_ = nullptr;
    QLabel* tipLabel_ = nullptr;
};