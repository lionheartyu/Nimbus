#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include <QThread>
#include <QListWidget> // 新增
#include <QString>

class FileClientWindow : public QWidget
{
    Q_OBJECT
public:
    explicit FileClientWindow(QWidget* parent = nullptr);

    void setToken(const QString& token) { token_ = token; }

    QTextEdit *logEdit;
    void uploadDirectory(const QString &rootDir, const QString &currentDir);
    void uploadFileWithRelativePath(const QString &absPath, const QString &relPath);

private slots:
    void onBrowse();
    void onUpload();
    void onList();
    void onDownload();
    void onFileClicked(QListWidgetItem *item);
    void onFileDoubleClicked(QListWidgetItem *item); // 新增的槽函数
    void onListContextMenu(const QPoint &pos);
    void onRecycle();

private:
    QString token_;

    QLineEdit *filePathEdit;
    QPushButton *browseBtn;
    QPushButton *uploadBtn;
    QPushButton *listBtn;
    QPushButton *downloadBtn;
    QPushButton *recycleBtn;
    QProgressBar *progressBar;
    QListWidget *fileListWidget;
    bool inRecycle = false;
};