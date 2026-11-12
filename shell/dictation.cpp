#include "dictation.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLibrary>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <atomic>
#include <utility>

namespace {
constexpr int sampleRate = 16000;
constexpr int audioLimit = sampleRate * 2 * 60;

QDir speechDirectory()
{
    return QDir(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../share/orbit-native-shell/speech"));
}

QString recognize(const QByteArray &audio, const QString &language, const std::atomic<bool> &cancelled)
{
    const auto directory = speechDirectory();
    QLibrary library(directory.filePath("libvosk.so"));
    using NewModel = void *(*)(const char *);
    using Free = void (*)(void *);
    using NewRecognizer = void *(*)(void *, float);
    using Waveform = int (*)(void *, const char *, int);
    using Result = const char *(*)(void *);
    const auto newModel = reinterpret_cast<NewModel>(library.resolve("vosk_model_new"));
    const auto freeModel = reinterpret_cast<Free>(library.resolve("vosk_model_free"));
    const auto newRecognizer = reinterpret_cast<NewRecognizer>(library.resolve("vosk_recognizer_new"));
    const auto freeRecognizer = reinterpret_cast<Free>(library.resolve("vosk_recognizer_free"));
    const auto accept = reinterpret_cast<Waveform>(library.resolve("vosk_recognizer_accept_waveform"));
    const auto result = reinterpret_cast<Result>(library.resolve("vosk_recognizer_result"));
    const auto finalResult = reinterpret_cast<Result>(library.resolve("vosk_recognizer_final_result"));
    if (!newModel || !freeModel || !newRecognizer || !freeRecognizer || !accept || !result || !finalResult) return {};

    const auto path = QFile::encodeName(directory.filePath(language));
    std::unique_ptr<void, Free> model(newModel(path.constData()), freeModel);
    if (!model || cancelled) return {};
    std::unique_ptr<void, Free> recognizer(newRecognizer(model.get(), sampleRate), freeRecognizer);
    if (!recognizer) return {};

    QStringList parts;
    const auto collect = [&parts](const char *value) {
        const auto text = QJsonDocument::fromJson(value).object().value("text").toString();
        if (!text.isEmpty()) parts.append(text);
    };
    for (qsizetype offset = 0; offset < audio.size() && !cancelled; offset += 6400) {
        const int length = int(qMin<qsizetype>(6400, audio.size() - offset));
        const auto status = accept(recognizer.get(), audio.constData() + offset, length);
        if (status < 0) return {};
        if (status > 0) collect(result(recognizer.get()));
    }
    if (!cancelled) collect(finalResult(recognizer.get()));
    return parts.join(' ');
}

class DictationDialog final : public QDialog {
public:
    explicit DictationDialog(QString executable) : executable_(std::move(executable))
    {
        setWindowTitle(QStringLiteral("Orbit Native — диктовка"));
        resize(465, 290);
        setWindowFlag(Qt::WindowStaysOnTopHint);
        auto *panel = new QVBoxLayout(this);
        auto *actions = new QHBoxLayout;
        language_.addItem(QStringLiteral("Русский"), "ru");
        language_.addItem("English", "en");
        toggle_.setText(QStringLiteral("Начать запись"));
        copy_.setText(QStringLiteral("Копировать"));
        copy_.setEnabled(false);
        actions->addWidget(&language_);
        actions->addWidget(&toggle_);
        actions->addStretch();
        actions->addWidget(&copy_);
        panel->addLayout(actions);
        panel->addWidget(&status_);
        panel->addWidget(&text_);
        status_.setText(QStringLiteral("Запишите до минуты речи. Обработка происходит на этом компьютере."));
        limit_.setSingleShot(true);
        connect(&limit_, &QTimer::timeout, this, &DictationDialog::stopRecording);
        status_.setWordWrap(true);
        connect(&toggle_, &QPushButton::clicked, this, [this] {
            if (capturing_) stopRecording();
            else startRecording();
        });
        connect(&copy_, &QPushButton::clicked, this, [this] {
            QApplication::clipboard()->setText(text_.toPlainText());
            status_.setText(QStringLiteral("Текст скопирован"));
        });
        connect(&recorder_, &QProcess::readyReadStandardOutput, this, [this] {
            const auto chunk = recorder_.readAllStandardOutput();
            if (!capturing_ || !isVisible()) return;
            audio_.append(chunk.left(qMax<qsizetype>(0, audioLimit - audio_.size())));
            if (audio_.size() >= audioLimit) stopRecording();
        });
        connect(&recorder_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError issue) {
            if (issue == QProcess::FailedToStart) captureFailed();
        });
        connect(&recorder_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this] {
            if (!capturing_) return;
            captureFailed();
        });
    }

    ~DictationDialog() override
    {
        cancelled_ = true;
        if (recorder_.state() != QProcess::NotRunning) recorder_.kill();
        worker_.waitForDone();
    }

protected:
    void hideEvent(QHideEvent *event) override
    {
        if (capturing_) {
            capturing_ = false;
            limit_.stop();
            audio_.clear();
            recorder_.kill();
            setAvailable();
        }
        QDialog::hideEvent(event);
    }

private:
    void setAvailable()
    {
        toggle_.setText(QStringLiteral("Начать запись"));
        toggle_.setEnabled(true);
        language_.setEnabled(true);
    }

    void startRecording()
    {
        if (recorder_.state() != QProcess::NotRunning || transcribing_) return;
        audio_.clear();
        capturing_ = true;
        limit_.start(60000);
        language_.setEnabled(false);
        toggle_.setText(QStringLiteral("Остановить запись"));
        status_.setText(QStringLiteral("Идёт запись с микрофона…"));
        recorder_.start(executable_, {"--raw", "--format=s16le", "--rate=16000", "--channels=1", "--latency-msec=100"});
    }

    void captureFailed()
    {
        capturing_ = false;
        limit_.stop();
        audio_.clear();
        setAvailable();
        status_.setText(QStringLiteral("Не удалось открыть микрофон"));
    }

    void stopRecording()
    {
        if (!capturing_) return;
        capturing_ = false;
        limit_.stop();
        recorder_.terminate();
        auto audio = std::exchange(audio_, QByteArray());
        audio.chop(audio.size() % 2);
        if (audio.size() < sampleRate) {
            setAvailable();
            status_.setText(QStringLiteral("Запись слишком короткая"));
            return;
        }
        const auto language = language_.currentData().toString();
        toggle_.setEnabled(false);
        transcribing_ = true;
        status_.setText(QStringLiteral("Распознаём речь…"));
        worker_.start([this, audio = std::move(audio), language] {
            const auto result = recognize(audio, language, cancelled_);
            if (cancelled_) return;
            QMetaObject::invokeMethod(this, [this, result] {
                transcribing_ = false;
                setAvailable();
                if (result.isEmpty()) {
                    status_.setText(QStringLiteral("Речь не распознана"));
                    return;
                }
                text_.setPlainText(result);
                copy_.setEnabled(true);
                status_.setText(QStringLiteral("Готово"));
            }, Qt::QueuedConnection);
        });
    }

    QString executable_;
    QComboBox language_;
    QPushButton toggle_, copy_;
    QLabel status_;
    QPlainTextEdit text_;
    QProcess recorder_;
    QTimer limit_;
    QByteArray audio_;
    std::atomic<bool> cancelled_ = false;
    QThreadPool worker_;
    bool capturing_ = false;
    bool transcribing_ = false;
};
}

Dictation::Dictation() = default;
Dictation::~Dictation() = default;

bool Dictation::open()
{
    if (!dialog_) {
        const auto executable = QStandardPaths::findExecutable("parecord");
        const auto directory = speechDirectory();
        if (executable.isEmpty() || !directory.exists("libvosk.so") ||
            !directory.exists("ru") || !directory.exists("en")) return false;
        dialog_ = std::make_unique<DictationDialog>(executable);
    }
    dialog_->show();
    dialog_->raise();
    dialog_->activateWindow();
    return true;
}

bool Dictation::test(const QString &pcmFile)
{
    QFile file(pcmFile);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const std::atomic<bool> cancelled = false;
    return recognize(file.read(audioLimit), "en", cancelled).contains("test", Qt::CaseInsensitive);
}
