#include "VeloxQtPlayerWindow.h"
#include "VeloxQtPlayerEngine.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QBoxLayout>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QUrl>
#include <QDirIterator>
#include <QStatusBar>

VeloxQtPlayerWindow::VeloxQtPlayerWindow(QWidget *parent)
    : QMainWindow(parent),
      engine(new VeloxQtPlayerEngine(this)),
      coverLabel(nullptr),
      titleLabel(nullptr),
      artistLabel(nullptr),
      infoLabel(nullptr),
      timeLabel(nullptr),
      playlist(nullptr),
      seekSlider(nullptr),
      loopButton(nullptr),
      prevButton(nullptr),
      playButton(nullptr),
      nextButton(nullptr),
      openFileButton(nullptr),
      openFolderButton(nullptr),
      uiTimer(nullptr),
      loopEnabled(false),
      currentIndex(-1)
{
    setWindowTitle("Velox Player");
    setAcceptDrops(true);
    buildUi();
    wireUi();
    updateCoverPlaceholder();

    uiTimer = new QTimer(this);
    connect(uiTimer, &QTimer::timeout, this, &VeloxQtPlayerWindow::updateProgress);
    uiTimer->start(200);
}

void VeloxQtPlayerWindow::addFiles(const QStringList &paths)
{
    QStringList added;
    for (const QString &path : paths)
    {
        if (!QFileInfo::exists(path))
            continue;
        if (!path.endsWith(".vlx", Qt::CaseInsensitive))
            continue;
        QString name = QFileInfo(path).fileName();
        auto *item = new QListWidgetItem(name);
        item->setData(Qt::UserRole, path);
        playlist->addItem(item);
        added.push_back(path);
    }

    if (!engine->isPlaying() && currentIndex < 0 && playlist->count() > 0)
    {
        playIndex(0);
    }
}

void VeloxQtPlayerWindow::playIndex(int index)
{
    if (index < 0 || index >= playlist->count())
        return;
    auto *item = playlist->item(index);
    if (!item)
        return;
    QString path = item->data(Qt::UserRole).toString();
    if (path.isEmpty())
        return;
    if (engine->playFile(path))
    {
        currentIndex = index;
        playlist->setCurrentRow(index);
        updateState();
        updateMetadata();
    }
}

void VeloxQtPlayerWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;
    for (const QUrl &url : event->mimeData()->urls())
    {
        if (url.toLocalFile().endsWith(".vlx", Qt::CaseInsensitive))
        {
            event->acceptProposedAction();
            return;
        }
    }
}

void VeloxQtPlayerWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls())
    {
        QString path = url.toLocalFile();
        if (path.endsWith(".vlx", Qt::CaseInsensitive))
            paths.push_back(path);
    }
    addFiles(paths);
    event->acceptProposedAction();
}

void VeloxQtPlayerWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);

    auto *topLayout = new QHBoxLayout();
    coverLabel = new QLabel();
    coverLabel->setFixedSize(100, 100);
    coverLabel->setAlignment(Qt::AlignCenter);
    coverLabel->setStyleSheet("background:#2f2f36;color:#9be7ff;");

    auto *infoLayout = new QVBoxLayout();
    titleLabel = new QLabel("Velox Player");
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);

    artistLabel = new QLabel("Drag and drop .vlx files");
    infoLabel = new QLabel("--");

    infoLayout->addStretch(1);
    infoLayout->addWidget(titleLabel);
    infoLayout->addWidget(artistLabel);
    infoLayout->addWidget(infoLabel);
    infoLayout->addStretch(1);

    topLayout->addWidget(coverLabel);
    topLayout->addLayout(infoLayout);
    topLayout->addStretch(1);

    playlist = new QListWidget();
    playlist->setSelectionMode(QAbstractItemView::SingleSelection);

    seekSlider = new QSlider(Qt::Horizontal);
    seekSlider->setRange(0, 1000);
    timeLabel = new QLabel("0:00 / 0:00");
    timeLabel->setAlignment(Qt::AlignCenter);

    auto *sliderLayout = new QVBoxLayout();
    sliderLayout->addWidget(seekSlider);
    sliderLayout->addWidget(timeLabel);

    auto *controlsLayout = new QHBoxLayout();
    loopButton = new QPushButton("Loop: Off");
    prevButton = new QPushButton("Prev");
    playButton = new QPushButton("Play");
    nextButton = new QPushButton("Next");
    openFileButton = new QPushButton("Open Files");
    openFolderButton = new QPushButton("Open Folder");
    controlsLayout->addWidget(loopButton);
    controlsLayout->addStretch(1);
    controlsLayout->addWidget(prevButton);
    controlsLayout->addWidget(playButton);
    controlsLayout->addWidget(nextButton);
    controlsLayout->addStretch(1);
    controlsLayout->addWidget(openFileButton);
    controlsLayout->addWidget(openFolderButton);

    rootLayout->addLayout(topLayout);
    rootLayout->addWidget(playlist);
    rootLayout->addLayout(sliderLayout);
    rootLayout->addLayout(controlsLayout);

    setCentralWidget(central);
    statusBar();
}

void VeloxQtPlayerWindow::wireUi()
{
    connect(openFileButton, &QPushButton::clicked, this, &VeloxQtPlayerWindow::openFiles);
    connect(openFolderButton, &QPushButton::clicked, this, &VeloxQtPlayerWindow::openFolder);
    connect(loopButton, &QPushButton::clicked, this, &VeloxQtPlayerWindow::toggleLoop);
    connect(prevButton, &QPushButton::clicked, this, &VeloxQtPlayerWindow::playPrev);
    connect(nextButton, &QPushButton::clicked, this, &VeloxQtPlayerWindow::playNext);
    connect(playButton, &QPushButton::clicked, this, &VeloxQtPlayerWindow::playSelected);

    connect(playlist, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!item)
            return;
        int idx = playlist->row(item);
        playIndex(idx);
    });

    connect(seekSlider, &QSlider::sliderReleased, this, [this]() {
        qint64 total = engine->totalFrames();
        if (total <= 0)
            return;
        int pos = seekSlider->value();
        qint64 target = static_cast<qint64>((static_cast<double>(pos) / 1000.0) * total);
        engine->seekFrame(target);
    });

    connect(engine, &VeloxQtPlayerEngine::metadataChanged, this, &VeloxQtPlayerWindow::updateMetadata);
    connect(engine, &VeloxQtPlayerEngine::stateChanged, this, &VeloxQtPlayerWindow::updateState);
    connect(engine, &VeloxQtPlayerEngine::playbackFinished, this, &VeloxQtPlayerWindow::playNext);
    connect(engine, &VeloxQtPlayerEngine::errorOccurred, this, [this](const QString &msg) {
        statusBar()->showMessage(msg, 5000);
    });
}

void VeloxQtPlayerWindow::updateMetadata()
{
    titleLabel->setText(engine->title());
    artistLabel->setText(engine->artist());
    QString infoText = engine->info();
    QString bitrateText = engine->bitrate();
    if (!bitrateText.isEmpty())
        infoText = infoText + " | " + bitrateText;
    infoLabel->setText(infoText);

    QImage cover = engine->coverArt();
    if (!cover.isNull())
    {
        QPixmap pix = QPixmap::fromImage(cover).scaled(100, 100, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        coverLabel->setPixmap(pix);
        coverLabel->setText(QString());
    }
    else
    {
        coverLabel->setPixmap(QPixmap());
        updateCoverPlaceholder();
    }
    setWindowTitle("Velox Player - " + engine->title());
}

void VeloxQtPlayerWindow::updateState()
{
    if (!engine->isPlaying())
        playButton->setText("Play");
    else if (engine->isPaused())
        playButton->setText("Play");
    else
        playButton->setText("Pause");

    loopButton->setText(loopEnabled ? "Loop: On" : "Loop: Off");
}

void VeloxQtPlayerWindow::updateProgress()
{
    qint64 total = engine->totalFrames();
    if (total <= 0)
        return;
    qint64 current = engine->currentFrame();
    int pos = static_cast<int>((static_cast<double>(current) / static_cast<double>(total)) * 1000.0);
    if (!seekSlider->isSliderDown())
        seekSlider->setValue(pos);

    uint32_t sr = engine->sampleRate();
    if (sr > 0)
    {
        qint64 curSec = current / sr;
        qint64 totalSec = total / sr;
        QString curText = QString("%1:%2").arg(curSec / 60).arg(curSec % 60, 2, 10, QChar('0'));
        QString totalText = QString("%1:%2").arg(totalSec / 60).arg(totalSec % 60, 2, 10, QChar('0'));
        timeLabel->setText(curText + " / " + totalText);
    }
}

void VeloxQtPlayerWindow::updateCoverPlaceholder()
{
    coverLabel->setStyleSheet("background:#2f2f36;color:#9be7ff;");
    coverLabel->setText("V");
    QFont f = coverLabel->font();
    f.setPointSize(36);
    f.setBold(true);
    coverLabel->setFont(f);
}

void VeloxQtPlayerWindow::playSelected()
{
    if (engine->isPlaying())
    {
        engine->togglePause();
        updateState();
        return;
    }

    int idx = playlist->currentRow();
    if (idx < 0 && playlist->count() > 0)
        idx = 0;
    playIndex(idx);
}

void VeloxQtPlayerWindow::playNext()
{
    if (playlist->count() == 0)
        return;
    int next = currentIndex + 1;
    if (next >= playlist->count())
    {
        if (loopEnabled)
            next = 0;
        else
            return;
    }
    playIndex(next);
}

void VeloxQtPlayerWindow::playPrev()
{
    if (playlist->count() == 0)
        return;
    int prev = currentIndex - 1;
    if (prev < 0)
    {
        if (loopEnabled)
            prev = playlist->count() - 1;
        else
            return;
    }
    playIndex(prev);
}

void VeloxQtPlayerWindow::openFiles()
{
    QStringList files = QFileDialog::getOpenFileNames(this, "Open Velox Files", QString(), "Velox Files (*.vlx)");
    addFiles(files);
}

void VeloxQtPlayerWindow::openFolder()
{
    QString dir = QFileDialog::getExistingDirectory(this, "Open Folder", QString());
    if (dir.isEmpty())
        return;
    QStringList files;
    QDirIterator it(dir, QStringList() << "*.vlx", QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files.push_back(it.next());
    addFiles(files);
}

void VeloxQtPlayerWindow::toggleLoop()
{
    loopEnabled = !loopEnabled;
    updateState();
}
