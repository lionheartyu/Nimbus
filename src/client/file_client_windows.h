#pragma once
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include <QThread>
#include <QListWidget> // 新增

class FileClientWindow : public QWidget {
    Q_OBJECT
public:
    FileClientWindow(QWidget *parent = nullptr);

    // 让这三个成员 public
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