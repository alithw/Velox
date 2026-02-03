#ifndef VELOX_QT_PLAYER_ENGINE_H
#define VELOX_QT_PLAYER_ENGINE_H

#include <QObject>
#include <QImage>
#include <QAudio>
#include <QAudioFormat>
#include <QString>

#include <atomic>
#include <thread>
#include <vector>

#include "VeloxCore.h"
#include "VeloxMetadata.h"
#include "VeloxArch.h"

class AudioBufferDevice;
class QAudioSink;

class VeloxQtPlayerEngine : public QObject
{
    Q_OBJECT

public:
    explicit VeloxQtPlayerEngine(QObject *parent = nullptr);
    ~VeloxQtPlayerEngine();

    bool playFile(const QString &path);
    void stop();
    void pause();
    void resume();
    void togglePause();
    void seekFrame(qint64 frame);

    bool isPlaying() const;
    bool isPaused() const;
    bool hasTrack() const;

    qint64 currentFrame() const;
    qint64 totalFrames() const;
    uint32_t sampleRate() const;
    uint16_t channels() const;
    uint16_t bitsPerSample() const;

    QString title() const;
    QString artist() const;
    QString info() const;
    QString bitrate() const;
    QImage coverArt() const;
    QString filePath() const;

signals:
    void metadataChanged();
    void stateChanged();
    void playbackFinished();
    void errorOccurred(const QString &message);

private:
    bool startAudio();
    bool waitForPrebuffer(int timeoutMs);
    void stopAudio();
    bool loadFile(const QString &path);
    void startDecode(uint64_t session);
    void decodeLoop(uint64_t session);
    void handleAudioStateChanged(QAudio::State state);

    static int16_t convertSample(velox_sample_t raw, uint8_t exp, bool isFloat, int floatMode, int bits);

    AudioBufferDevice *audioDevice;
    QAudioSink *audioSink;
    QAudioFormat audioFormat;

    std::thread decodeThread;
    std::atomic<bool> stopRequested;
    std::atomic<bool> seekRequested;
    std::atomic<bool> paused;
    std::atomic<bool> playing;

    std::atomic<qint64> currentFrameAtomic;
    std::atomic<qint64> totalFramesAtomic;
    std::atomic<uint32_t> sampleRateAtomic;
    std::atomic<qint64> framesPlayedAtomic;
    std::atomic<qint64> pauseFrameAtomic;
    std::atomic<uint64_t> sessionCounter;
    std::atomic<uint64_t> activeSession;
    std::atomic<uint64_t> audioSession;

    std::atomic<qint64> seekTargetFrame;
    uint64_t totalSamplesValue;
    uint16_t channelsValue;
    uint16_t bitsPerSampleValue;
    uint16_t formatCodeValue;
    bool isFloatValue;

    QString titleValue;
    QString artistValue;
    QString infoValue;
    QString bitrateValue;
    QString filePathValue;
    QImage coverArtValue;

    std::vector<uint8_t> compData;
    size_t bytesPerFrame;
    size_t prebufferBytes;
};

#endif
