#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include <QThread>
#include <QListWidget> // 新增
#include <QString>

/// Nimbus 文件客户端主窗口类
class FileClientWindow : public QWidget
{
    Q_OBJECT
public:
    /// 构造函数
    explicit FileClientWindow(QWidget *parent = nullptr);

    /// 设置登录 token
    void setToken(const QString &token) { token_ = token; }

    /// 日志输出控件（公有，便于外部追加日志）
    QTextEdit *logEdit;

    /// 递归上传目录
    void uploadDirectory(const QString &rootDir, const QString &currentDir);

    /// 上传单个文件（带相对路径）
    void uploadFileWithRelativePath(const QString &absPath, const QString &relPath);

private slots:
    /// 选择文件按钮槽函数
    void onBrowse();

    /// 上传按钮槽函数
    void onUpload();

    /// 列表按钮槽函数（显示云端文件列表）
    void onList();

    /// 下载按钮槽函数
    void onDownload();

    /// 文件列表项点击槽函数
    void onFileClicked(QListWidgetItem *item);

    /// 文件列表项双击槽函数（预览/播放/外链）
    void onFileDoubleClicked(QListWidgetItem *item); // 新增的槽函数

    /// 文件列表右键菜单槽函数
    void onListContextMenu(const QPoint &pos);

    /// 回收站按钮槽函数
    void onRecycle();

    // 文件搜索按钮槽函数
    void onSearch();

    // 登出按钮槽函数
    void onLogout();

private:
    QString token_;      ///< 当前登录用户的 token
    QString currentDir_; ///< 当前浏览的目录前缀，默认为空字符串
    QString getFullPath_(const QString &name) const
    {
        return currentDir_.isEmpty() ? name : (currentDir_ + name);
    }
    // UI 控件成员
    QLineEdit *filePathEdit;     ///< 文件路径输入框
    QPushButton *browseBtn;      ///< 选择文件按钮
    QPushButton *uploadBtn;      ///< 上传按钮
    QPushButton *listBtn;        ///< 云端文件列表按钮
    QPushButton *downloadBtn;    ///< 下载按钮
    QPushButton *recycleBtn;     ///< 回收站按钮
    QProgressBar *progressBar;   ///< 上传/下载进度条
    QListWidget *fileListWidget; ///< 文件列表控件
    QLineEdit *searchEdit = nullptr;// 文件搜索输入框
    QPushButton *searchBtn = nullptr;// 文件搜索按钮
    bool inRecycle = false; ///< 是否处于回收站视图
    QPushButton *logoutBtn = nullptr; ///< 登出按钮
};