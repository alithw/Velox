#ifndef VELOX_QT_PLAYER_WINDOW_H
#define VELOX_QT_PLAYER_WINDOW_H

#include <QMainWindow>
#include <QStringList>

class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QTimer;
class QCloseEvent;
class VeloxQtPlayerEngine;

class VeloxQtPlayerWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit VeloxQtPlayerWindow(QWidget *parent = nullptr);
    void addFiles(const QStringList &paths);
    void playIndex(int index);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void wireUi();
    void updateMetadata();
    void updateState();
    void updateProgress();
    void updateCoverPlaceholder();
    void playSelected();
    void playNext();
    void playPrev();
    void openFiles();
    void openFolder();
    void toggleLoop();

    VeloxQtPlayerEngine *engine;
    QLabel *coverLabel;
    QLabel *titleLabel;
    QLabel *artistLabel;
    QLabel *infoLabel;
    QLabel *timeLabel;
    QListWidget *playlist;
    QSlider *seekSlider;
    QPushButton *loopButton;
    QPushButton *prevButton;
    QPushButton *playButton;
    QPushButton *nextButton;
    QPushButton *openFileButton;
    QPushButton *openFolderButton;
    QTimer *uiTimer;
    bool loopEnabled;
    int currentIndex;
    bool closing;
};

#endif
