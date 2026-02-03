#include "VeloxQtPlayerEngine.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QFileInfo>
#include <QMediaDevices>
#include <QIODevice>
#include <QByteArray>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

namespace {
class AudioRingBuffer
{
public:
    void resetBuffer(size_t capacityBytes)
    {
        std::lock_guard<std::mutex> lock(mutex);
        capacity = capacityBytes + 1;
        buffer.assign(capacity, 0);
        head = 0;
        tail = 0;
        finished = false;
        canceled = false;
        cv_read.notify_all();
        cv_write.notify_all();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        head = 0;
        tail = 0;
        finished = false;
        canceled = false;
        cv_read.notify_all();
        cv_write.notify_all();
    }

    bool push(const uint8_t *data, size_t len)
    {
        size_t written = 0;
        while (written < len)
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv_write.wait(lock, [&] { return canceled || freeSpaceLocked() > 0; });
            if (canceled)
                return false;
            size_t freeSpace = freeSpaceLocked();
            size_t chunk = std::min(len - written, freeSpace);
            size_t first = std::min(chunk, capacity - head);
            std::memcpy(buffer.data() + head, data + written, first);
            head = (head + first) % capacity;
            written += first;
            if (first < chunk)
            {
                size_t second = chunk - first;
                std::memcpy(buffer.data() + head, data + written, second);
                head = (head + second) % capacity;
                written += second;
            }
            cv_read.notify_one();
        }
        return true;
    }

    size_t read(uint8_t *out, size_t len)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv_read.wait(lock, [&] { return canceled || availableLocked() > 0 || finished; });
        if (canceled)
            return 0;
        size_t avail = availableLocked();
        if (avail == 0)
            return 0;
        size_t chunk = std::min(len, avail);
        size_t first = std::min(chunk, capacity - tail);
        std::memcpy(out, buffer.data() + tail, first);
        tail = (tail + first) % capacity;
        size_t readBytes = first;
        if (first < chunk)
        {
            size_t second = chunk - first;
            std::memcpy(out + first, buffer.data() + tail, second);
            tail = (tail + second) % capacity;
            readBytes += second;
        }
        cv_write.notify_one();
        return readBytes;
    }

    size_t available() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return availableLocked();
    }

    bool drained() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return finished && availableLocked() == 0;
    }

    void setFinished()
    {
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
        cv_read.notify_all();
        cv_write.notify_all();
    }

    void cancel()
    {
        std::lock_guard<std::mutex> lock(mutex);
        canceled = true;
        finished = true;
        cv_read.notify_all();
        cv_write.notify_all();
    }

private:
    size_t availableLocked() const
    {
        if (capacity == 0)
            return 0;
        if (head >= tail)
            return head - tail;
        return capacity - (tail - head);
    }

    size_t freeSpaceLocked() const
    {
        if (capacity == 0)
            return 0;
        return capacity - availableLocked() - 1;
    }

    size_t capacity = 0;
    std::vector<uint8_t> buffer;
    size_t head = 0;
    size_t tail = 0;
    bool finished = false;
    bool canceled = false;
    mutable std::mutex mutex;
    std::condition_variable cv_read;
    std::condition_variable cv_write;
};

}

class AudioBufferDevice : public QIODevice
{
public:
    explicit AudioBufferDevice(QObject *parent = nullptr)
        : QIODevice(parent)
    {
    }

    void resetBuffer(size_t capacityBytes)
    {
        ring.resetBuffer(capacityBytes);
        if (!isOpen())
            open(QIODevice::ReadOnly);
    }

    void clear()
    {
        ring.clear();
    }

    bool push(const uint8_t *data, size_t len)
    {
        return ring.push(data, len);
    }

    void setFinished()
    {
        ring.setFinished();
    }

    void cancel()
    {
        ring.cancel();
    }

    bool drained() const
    {
        return ring.drained();
    }

    size_t bufferedBytes() const
    {
        return ring.available();
    }

protected:
    qint64 readData(char *data, qint64 maxlen) override
    {
        if (maxlen <= 0)
            return 0;
        return static_cast<qint64>(ring.read(reinterpret_cast<uint8_t *>(data), static_cast<size_t>(maxlen)));
    }

    qint64 writeData(const char *, qint64) override
    {
        return -1;
    }

    qint64 bytesAvailable() const override
    {
        return static_cast<qint64>(ring.available()) + QIODevice::bytesAvailable();
    }

private:
    AudioRingBuffer ring;
};

VeloxQtPlayerEngine::VeloxQtPlayerEngine(QObject *parent)
    : QObject(parent),
      audioDevice(new AudioBufferDevice(this)),
      audioSink(nullptr),
      stopRequested(false),
      seekRequested(false),
      paused(false),
      playing(false),
      currentFrameAtomic(0),
      totalFramesAtomic(0),
      sampleRateAtomic(0),
      seekTargetFrame(0),
      totalSamplesValue(0),
      channelsValue(0),
      bitsPerSampleValue(0),
      formatCodeValue(0),
      isFloatValue(false)
{
}

VeloxQtPlayerEngine::~VeloxQtPlayerEngine()
{
    stop();
}

bool VeloxQtPlayerEngine::playFile(const QString &path)
{
    stop();
    if (!loadFile(path))
        return false;
    if (!startAudio())
        return false;
    startDecode();
    emit metadataChanged();
    emit stateChanged();
    return true;
}

void VeloxQtPlayerEngine::stop()
{
    stopRequested = true;
    paused = false;
    if (audioSink)
        audioSink->stop();
    if (audioDevice)
        audioDevice->cancel();
    if (decodeThread.joinable())
        decodeThread.join();
    stopAudio();
    playing = false;
    currentFrameAtomic = 0;
    stopRequested = false;
    emit stateChanged();
}

void VeloxQtPlayerEngine::pause()
{
    if (!playing || paused)
        return;
    paused = true;
    if (audioSink)
        audioSink->suspend();
    emit stateChanged();
}

void VeloxQtPlayerEngine::resume()
{
    if (!playing || !paused)
        return;
    paused = false;
    if (audioSink)
        audioSink->resume();
    emit stateChanged();
}

void VeloxQtPlayerEngine::togglePause()
{
    if (!playing)
        return;
    if (paused)
        resume();
    else
        pause();
}

void VeloxQtPlayerEngine::seekFrame(qint64 frame)
{
    if (!playing || totalFramesAtomic == 0)
        return;
    if (frame < 0)
        frame = 0;
    if (frame >= totalFramesAtomic)
        frame = totalFramesAtomic - 1;
    seekTargetFrame = frame;
    currentFrameAtomic = frame;
    seekRequested = true;
    if (audioDevice)
        audioDevice->clear();
}

bool VeloxQtPlayerEngine::isPlaying() const
{
    return playing.load();
}

bool VeloxQtPlayerEngine::isPaused() const
{
    return paused.load();
}

bool VeloxQtPlayerEngine::hasTrack() const
{
    return !filePathValue.isEmpty();
}

qint64 VeloxQtPlayerEngine::currentFrame() const
{
    return currentFrameAtomic.load();
}

qint64 VeloxQtPlayerEngine::totalFrames() const
{
    return totalFramesAtomic.load();
}

uint32_t VeloxQtPlayerEngine::sampleRate() const
{
    return sampleRateAtomic.load();
}

uint16_t VeloxQtPlayerEngine::channels() const
{
    return channelsValue;
}

uint16_t VeloxQtPlayerEngine::bitsPerSample() const
{
    return bitsPerSampleValue;
}

QString VeloxQtPlayerEngine::title() const
{
    return titleValue;
}

QString VeloxQtPlayerEngine::artist() const
{
    return artistValue;
}

QString VeloxQtPlayerEngine::info() const
{
    return infoValue;
}

QString VeloxQtPlayerEngine::bitrate() const
{
    return bitrateValue;
}

QImage VeloxQtPlayerEngine::coverArt() const
{
    return coverArtValue;
}

QString VeloxQtPlayerEngine::filePath() const
{
    return filePathValue;
}

bool VeloxQtPlayerEngine::startAudio()
{
    stopAudio();
    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    audioFormat.setSampleRate(static_cast<int>(sampleRateAtomic.load()));
    audioFormat.setChannelCount(static_cast<int>(channelsValue));
    audioFormat.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(audioFormat))
    {
        emit errorOccurred("Audio output does not support 16-bit PCM for this format.");
        return false;
    }

    audioSink = new QAudioSink(device, audioFormat, this);
    connect(audioSink, &QAudioSink::stateChanged, this, &VeloxQtPlayerEngine::handleAudioStateChanged);

    size_t bytesPerSecond = static_cast<size_t>(sampleRateAtomic.load()) * static_cast<size_t>(channelsValue) * sizeof(int16_t);
    size_t capacity = bytesPerSecond * 2;
    if (capacity < 256 * 1024)
        capacity = 256 * 1024;
    if (capacity > 8 * 1024 * 1024)
        capacity = 8 * 1024 * 1024;
    audioDevice->resetBuffer(capacity);
    audioSink->start(audioDevice);
    return true;
}

void VeloxQtPlayerEngine::stopAudio()
{
    if (audioSink)
    {
        audioSink->stop();
        delete audioSink;
        audioSink = nullptr;
    }
    if (audioDevice)
        audioDevice->close();
}

bool VeloxQtPlayerEngine::loadFile(const QString &path)
{
    std::string pathUtf8 = path.toUtf8().constData();
    std::ifstream in(pathUtf8.c_str(), std::ios::binary | std::ios::ate);
    if (!in.is_open())
    {
        emit errorOccurred("Unable to open file: " + path);
        return false;
    }

    size_t fileSize = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);

    VeloxHeader vh;
    in.read(reinterpret_cast<char *>(&vh), sizeof(vh));
    if (!in || vh.magic != 0x584C4556)
    {
        emit errorOccurred("Invalid Velox file: " + path);
        return false;
    }

    totalSamplesValue = vh.total_samples;
    channelsValue = vh.channels;
    bitsPerSampleValue = vh.bits_per_sample & 0x7FFF;
    formatCodeValue = vh.format_code;
    isFloatValue = (formatCodeValue == 3);

    if (channelsValue == 0 || vh.sample_rate == 0)
    {
        emit errorOccurred("Unsupported audio header in: " + path);
        return false;
    }

    sampleRateAtomic = vh.sample_rate;
    totalFramesAtomic = static_cast<qint64>(vh.total_samples / vh.channels);
    currentFrameAtomic = 0;
    seekTargetFrame = 0;

    VeloxMetadata meta;
    if (vh.version >= 0x0400)
        meta.ReadFromStream(in);

    QString fileName = QFileInfo(path).fileName();
    QString metaTitle = QString::fromStdString(meta.GetTag("TITLE"));
    QString metaArtist = QString::fromStdString(meta.GetTag("ARTIST"));
    titleValue = metaTitle.isEmpty() ? fileName : metaTitle;
    artistValue = metaArtist.isEmpty() ? QString("Unknown Artist") : metaArtist;

    coverArtValue = QImage();
    if (meta.hasCoverArt && !meta.coverArt.data.empty())
    {
        QByteArray bytes(reinterpret_cast<const char *>(meta.coverArt.data.data()),
                         static_cast<int>(meta.coverArt.data.size()));
        coverArtValue = QImage::fromData(bytes);
    }

    double srk = static_cast<double>(sampleRateAtomic.load()) / 1000.0;
    QString floatLabel = isFloatValue ? QString(" Float") : QString();
    infoValue = QString("%1bit / %2kHz%3").arg(bitsPerSampleValue).arg(srk, 0, 'f', 1).arg(floatLabel);

    QString bitrateText = QString("VLX -- kbps");
    if (totalFramesAtomic > 0)
    {
        double dur = static_cast<double>(totalFramesAtomic.load()) / static_cast<double>(sampleRateAtomic.load());
        if (dur > 0.0)
        {
            int kbps = static_cast<int>((fileSize * 8) / (dur * 1000.0));
            bitrateText = QString("VLX %1 kbps").arg(kbps);
        }
    }
    bitrateValue = bitrateText;

    filePathValue = path;

    in.seekg(static_cast<std::streamoff>(vh.header_blob_size), std::ios::cur);
    in.seekg(static_cast<std::streamoff>(vh.footer_blob_size), std::ios::cur);

    compData.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (compData.empty())
    {
        emit errorOccurred("No compressed audio found in: " + path);
        return false;
    }
    return true;
}

void VeloxQtPlayerEngine::startDecode()
{
    if (decodeThread.joinable())
        decodeThread.join();
    stopRequested = false;
    seekRequested = false;
    paused = false;
    playing = true;
    decodeThread = std::thread(&VeloxQtPlayerEngine::decodeLoop, this);
}

void VeloxQtPlayerEngine::decodeLoop()
{
    VeloxCodec::StreamingDecoder decoder(compData.data(), compData.size(), totalSamplesValue);
    int floatMode = decoder.GetFloatMode();
    bool isFloat = isFloatValue;
    int bits = bitsPerSampleValue;
    uint16_t ch = channelsValue;

    const int batchSize = 16384;
    std::vector<int16_t> pcmBatch;
    pcmBatch.reserve(batchSize);
    size_t samplesDecoded = 0;

    while (!stopRequested)
    {
        if (seekRequested)
        {
            if (audioDevice)
                audioDevice->clear();
            size_t targetSample = static_cast<size_t>(seekTargetFrame.load()) * ch;
            decoder = VeloxCodec::StreamingDecoder(compData.data(), compData.size(), totalSamplesValue);
            floatMode = decoder.GetFloatMode();
            samplesDecoded = 0;
            velox_sample_t dv;
            uint8_t de;
            while (samplesDecoded < targetSample && !stopRequested)
            {
                if (!decoder.DecodeNext(dv, de))
                    break;
                samplesDecoded++;
            }
            currentFrameAtomic = static_cast<qint64>(samplesDecoded / ch);
            seekRequested = false;
        }

        if (paused)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        pcmBatch.clear();
        for (int i = 0; i < batchSize; ++i)
        {
            velox_sample_t val;
            uint8_t exp;
            if (!decoder.DecodeNext(val, exp))
            {
                if (!pcmBatch.empty())
                {
                    if (!audioDevice->push(reinterpret_cast<const uint8_t *>(pcmBatch.data()), pcmBatch.size() * sizeof(int16_t)))
                        return;
                }
                audioDevice->setFinished();
                currentFrameAtomic = static_cast<qint64>(samplesDecoded / ch);
                return;
            }
            pcmBatch.push_back(convertSample(val, exp, isFloat, floatMode, bits));
            samplesDecoded++;
        }

        if (!audioDevice->push(reinterpret_cast<const uint8_t *>(pcmBatch.data()), pcmBatch.size() * sizeof(int16_t)))
            return;
        currentFrameAtomic = static_cast<qint64>(samplesDecoded / ch);
    }
}

void VeloxQtPlayerEngine::handleAudioStateChanged(QAudio::State state)
{
    if (stopRequested)
        return;
    if (state == QAudio::IdleState)
    {
        if (audioDevice && audioDevice->drained())
        {
            playing = false;
            paused = false;
            emit stateChanged();
            emit playbackFinished();
        }
    }
    else if (state == QAudio::StoppedState)
    {
        if (audioSink && audioSink->error() != QAudio::NoError)
            emit errorOccurred("Audio output error.");
    }
}

int16_t VeloxQtPlayerEngine::convertSample(velox_sample_t raw, uint8_t exp, bool isFloat, int floatMode, int bits)
{
    if (isFloat)
    {
        if (floatMode == 0)
        {
            velox_sample_t m = raw;
            uint32_t s = 0;
            if (m < 0)
            {
                s = 1;
                m = -m;
            }
            uint32_t ma = static_cast<uint32_t>(m & 0x7FFFFF);
            uint32_t u = (s << 31) | (static_cast<uint32_t>(exp) << 23) | ma;
            float f;
            std::memcpy(&f, &u, 4);
            if (std::isnan(f))
                f = 0.0f;
            if (f > 1.0f)
                f = 1.0f;
            if (f < -1.0f)
                f = -1.0f;
            return static_cast<int16_t>(f * 32767.0f);
        }
        return (floatMode == 1) ? static_cast<int16_t>(raw) : static_cast<int16_t>(raw >> 8);
    }
    if (bits == 24)
        return static_cast<int16_t>(raw >> 8);
    if (bits == 32)
        return static_cast<int16_t>(raw >> 16);
    return static_cast<int16_t>(raw);
}
