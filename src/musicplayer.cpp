#include "audiodecoder.h"
#include "imageprovider.h"
#include "musicplayer.h"

#include <QDebug>
#include <QAudioOutput>
#include <QFileInfo>
#include <QHash>
#include <QTimer>
#include <QUrl>

/**
 * @note ffmpeg所有可打开的字幕格式。
 *       因此，需要去掉它们。
 *       剩下的即为支持的音频和视频格式。
 */
static QHash<QString, bool> SubtitleFormat =
{
    {"srt", true}, {"SRT", true}, {"ssa", true},  {"SSA", true},
    {"ass", true}, {"ASS", true}, {"txt", true},  {"TXT", true},
    {"lrc", true}, {"LRC", true}, {"sup", true},  {"SUP", true},
    {"stl", true}, {"STL", true}, {"aqt", true},  {"AQT", true},
    {"smi", true}, {"SMI", true}, {"pjs", true},  {"PJS", true},
    {"rt", true},  {"RT", true},  {"sami", true}, {"SAMI", true}
};

class MusicPlayerPrivate
{
public:
    bool m_running = false;
    QUrl m_curMusic = QUrl();
    qreal m_progress = 0.0;
    qreal m_duration = 0.0;
    int m_volume = 100;
    QString m_title = QString();
    QString m_singer = QString();
    QString m_album = QString();
    QByteArray m_audioBuffer = QByteArray();
    QTimer *m_playTimer = nullptr;
    QScopedPointer<QAudioOutput> m_audioOutput;
    QIODevice *m_audioDevice = nullptr;
    AudioDecoder *m_decoder = nullptr;

    bool m_hasLyrics = false;
    QScopedPointer<LrcDecoder> m_lrcDecoder;
    LyricsModel *m_lyricsModel = nullptr;
    MusicModel *m_musicModel = nullptr;
    int m_lyricIndex = 0;
    int m_nextIndex = 0;
    /** @note 用于去重 */
    QHash<QString, bool> m_files;

    bool contains(const QString &filename) {
        if (m_files.value(filename)) {
            return true;
        }
        else {
            m_files[filename] = true;
            return false;
        }
    }

    ImageProvider *m_playbillProvider;
};

MusicPlayer::MusicPlayer(QObject *parent)
    : QObject (parent)
{
    d = new MusicPlayerPrivate;

    d->m_playbillProvider = new ImageProvider;
    d->m_playTimer = new QTimer(this);
    connect(d->m_playTimer, &QTimer::timeout, this, &MusicPlayer::update);

    d->m_lrcDecoder.reset(new LrcDecoder);

    d->m_lyricsModel = new LyricsModel(this);
    d->m_musicModel = new MusicModel(this);
    d->m_decoder = new AudioDecoder(this);
    connect(d->m_decoder, &AudioDecoder::error, this, &MusicPlayer::error);
    connect(d->m_decoder, &AudioDecoder::hasPlaybill, this, [this](const QImage &playbill) {
        d->m_playbillProvider->setImage(playbill);
        emit playbillChanged();
    });
    connect(d->m_decoder, &AudioDecoder::resolved, this, [this]() {
        d->m_running = true;
        d->m_title = d->m_decoder->title();
        d->m_singer = d->m_decoder->singer();
        d->m_album = d->m_decoder->album();
        d->m_duration = d->m_decoder->duration();
        emit titleChanged();
        emit singerChanged();
        emit albumChanged();
        emit durationChanged();

        d->m_audioOutput.reset(new QAudioOutput(d->m_decoder->format()));
        d->m_audioDevice = d->m_audioOutput->start();
        d->m_audioOutput->setVolume(d->m_volume / qreal(100.0));
        d->m_playTimer->start(100);
    });
}

MusicPlayer::~MusicPlayer()
{
    suspend();
    delete d;
}

ImageProvider *MusicPlayer::imageProvider()
{
    return d->m_playbillProvider;
}

QUrl MusicPlayer::curMusic() const
{
    return d->m_curMusic;
}

void MusicPlayer::setCurMusic(const QUrl &url)
{
    if (url != d->m_curMusic) {
        d->m_curMusic = url;
        emit curMusicChanged();
    }
}

qreal MusicPlayer::progress() const
{
    return d->m_progress;
}

void MusicPlayer::setProgress(qreal ratio)
{
    if (d->m_running && qAbs(ratio - d->m_progress) > 0.000001) {
        d->m_progress = ratio;
        d->m_audioBuffer.clear();
        emit progressChanged();
        d->m_decoder->setProgress(ratio);
        if (d->m_hasLyrics) {
            int64_t pts = int64_t(ratio * d->m_duration * 1000);
            int count = d->m_lyricsModel->count();
            for (int i = 0; i < count; i++) {
                if (d->m_lyricsModel->at(i)->pts() > pts) {
                    d->m_lyricIndex = (i > 0) ? (i - 1) : 0;
                    d->m_nextIndex = (d->m_lyricIndex + 1) < count ? d->m_lyricIndex + 1 : count;
                    emit lyricIndexChanged();
                    break;
                }
            }
        }
    }
}

int MusicPlayer::volume() const
{
    return d->m_volume;
}

void MusicPlayer::setVolume(int vol)
{
    if (vol != d->m_volume) {
        d->m_volume = vol;
        if (d->m_audioOutput) {
            d->m_audioOutput->setVolume(d->m_volume / qreal(100.0));
        }
        emit volumeChanged();
    }
}

qreal MusicPlayer::duration() const
{
    return d->m_duration;
}

bool MusicPlayer::running() const
{
    return d->m_running;
}

QString MusicPlayer::title() const
{
    return d->m_title;
}

QString MusicPlayer::singer() const
{
    return d->m_singer;
}

QString MusicPlayer::album() const
{
    return d->m_album;
}

int MusicPlayer::lyricIndex() const
{
    return d->m_lyricIndex;
}

LyricsModel* MusicPlayer::lyrics() const
{
    return d->m_lyricsModel;
}

MusicModel* MusicPlayer::music() const
{
    return d->m_musicModel;
}

void MusicPlayer::play(const QUrl &url)
{
    suspend();
    setCurMusic(url);
    d->m_running = false;
    d->m_progress = 0.0;
    emit progressChanged();
    d->m_audioBuffer.clear();
    d->m_hasLyrics = false;
    d->m_playbillProvider->setImage(QImage(":/image/music.png"));
    emit playbillChanged();

    QString filename = url.toLocalFile();
    d->m_decoder->open(filename);

    d->m_lyricIndex = 0;
    d->m_nextIndex = 0;
    int suffixLength = QFileInfo(filename).suffix().length();
    QString lrcFile = filename.mid(0, filename.length() - suffixLength - 1) + ".lrc";
    if (QFileInfo::exists(lrcFile)) {
        if (d->m_lrcDecoder->decode(lrcFile.toLocal8Bit().data())) {
            //创建Model
            QVector<LyricData *> model;
            lyricPacket packet = d->m_lrcDecoder->readPacket();
            while (!packet.isEmpty()) {
                LyricData *data = new LyricData(QString::fromStdString(packet.lyric), packet.pts);
                model.append(data);
                packet = d->m_lrcDecoder->readPacket();
            }
            d->m_lyricsModel->setModel(model);
            d->m_hasLyrics = true;
            if (d->m_nextIndex + 1 < d->m_lyricsModel->count()) d->m_nextIndex++;
            //打印LRC元数据
            d->m_lrcDecoder->dumpMetadata(stdout);
        }
    }
}

void MusicPlayer::suspend()
{
    if(d->m_playTimer->isActive())
        d->m_playTimer->stop();
}

void MusicPlayer::resume()
{
    if (d->m_running) d->m_playTimer->start(100);
    else if (!d->m_curMusic.isEmpty()) play(d->m_curMusic);
}

void MusicPlayer::addMusicList(const QList<QUrl> &urls)
{
    for (auto url: urls) {
        QString filename = url.toLocalFile();
        if (!SubtitleFormat.value(QFileInfo(filename).suffix())) {
            if (d->contains(filename)) {
                continue;
            } else {
                MusicData *data = MusicData::create(url, this);
                if (data) d->m_musicModel->append(data);
            }
        }
    }

    emit d->m_musicModel->modelChanged();
}

void MusicPlayer::update()
{
    while (d->m_audioBuffer.size() < d->m_audioOutput->bytesFree()) {
        AudioPacket packet = d->m_decoder->currentPacket();
        QByteArray data = packet.data;
        qreal currentTime = packet.time;
        if (currentTime >= d->m_duration || (data.isEmpty() && currentTime < 0.00000001)) {
            d->m_progress = 1.0;
            d->m_running = false;
            d->m_decoder->stop();
            d->m_playTimer->stop();
            emit finished();
            emit progressChanged();
            return;
        } else {
            if (d->m_hasLyrics) {
                int64_t pts = int64_t(currentTime) * 1000;
                if (pts > d->m_lyricsModel->at(d->m_lyricIndex)->pts() && pts > d->m_lyricsModel->at(d->m_nextIndex)->pts()) {
                    d->m_lyricIndex = d->m_nextIndex;
                    if ((d->m_nextIndex + 1) < d->m_lyricsModel->count()) d->m_nextIndex++;
                    emit lyricIndexChanged();
                }
            }
            d->m_progress = currentTime / d->m_duration;
            emit progressChanged();
        }

        if (data.isEmpty()) break;
        d->m_audioBuffer += data;
    }

    int readSize = d->m_audioOutput->periodSize();
    int chunks = d->m_audioBuffer.size() / readSize;
    while (chunks--) {
        QByteArray pcm = d->m_audioBuffer.mid(0, readSize);
        int size = pcm.size();
        d->m_audioBuffer.remove(0, size);

        if (size) d->m_audioDevice->write(pcm);
        if (size != readSize) break;
    }
}
