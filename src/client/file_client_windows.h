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

private slots:
    void onBrowse();
    void onUpload();
    void onList();
    void onDownload();
    void onFileClicked(QListWidgetItem *item);
    void onListContextMenu(const QPoint &pos);
    void onUploadFolder(); // 新增
    void onRecycle();        // 新增

private:
    QLineEdit *filePathEdit;
    QPushButton *browseBtn;
    QPushButton *uploadBtn;
    QPushButton *listBtn;
    QPushButton *downloadBtn;
    QPushButton *recycleBtn; // 新增
    QProgressBar *progressBar;
    QTextEdit *logEdit;
    QListWidget *fileListWidget; // 新增
    QPushButton *uploadFolderBtn; // 新增
    void uploadDirectory(const QString &rootDir, const QString &currentDir); // 新增
    void uploadFileWithRelativePath(const QString &absPath, const QString &relPath); // 新增
    bool inRecycle = false; // 标记当前是否在回收站
};