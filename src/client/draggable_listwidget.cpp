#include "draggable_listwidget.h"
#include "file_client_windows.h"

void DraggableListWidget::dropEvent(QDropEvent *event) {
    if (!mainWindow) return;
    const QMimeData *mimeData = event->mimeData();
    if (mimeData->hasUrls()) {
        QList<QUrl> urlList = mimeData->urls();
        for (const QUrl &url : urlList) {
            QString localPath = url.toLocalFile();
            QFileInfo info(localPath);
            if (info.isDir()) {
                mainWindow->uploadDirectory(localPath, localPath);
                mainWindow->logEdit->append("拖拽上传文件夹完成: " + localPath);
            } else if (info.isFile()) {
                mainWindow->uploadFileWithRelativePath(localPath, info.fileName());
            }
        }
    }
}