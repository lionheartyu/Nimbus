#pragma once
#include <QListWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFileInfo>
#include <QUrl>

class FileClientWindow; // 前向声明

class DraggableListWidget : public QListWidget {
    Q_OBJECT // 一定要加上这行！
public:
    explicit DraggableListWidget(QWidget *parent = nullptr) : QListWidget(parent), mainWindow(nullptr) {
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DropOnly);
    }
    void setMainWindow(FileClientWindow *w) { mainWindow = w; }
protected:
    void dragEnterEvent(QDragEnterEvent *event) override {
        if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    }
    void dropEvent(QDropEvent *event) override;
private:
    FileClientWindow *mainWindow;
};