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
    void onFileClicked(QListWidgetItem *item); // 新增
    void onListContextMenu(const QPoint &pos); // 新增

private:
    QLineEdit *filePathEdit;
    QPushButton *browseBtn;
    QPushButton *uploadBtn;
    QPushButton *listBtn;
    QPushButton *downloadBtn;
    QProgressBar *progressBar;
    QTextEdit *logEdit;
    QListWidget *fileListWidget; // 新增
};