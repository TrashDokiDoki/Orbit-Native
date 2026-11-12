#include "media.h"
#include "workspace.h"
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QSignalBlocker>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <functional>
#include <memory>
#include <utility>

class Island final : public QWidget {
public:
    explicit Island(bool testing)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    {
        setWindowTitle("Orbit Native");
        setWindowIcon(QIcon(":/art/icon.png"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAcceptDrops(true);

        root_ = new QVBoxLayout(this);
        root_->setContentsMargins(0, 0, 0, 0);
        card_ = new QWidget(this);
        card_->setObjectName("island");
        root_->addWidget(card_);
        auto *cardLayout = new QVBoxLayout(card_);
        cardLayout->setContentsMargins(9, 4, 9, 4);
        header_ = new QWidget(card_);
        auto *headerLayout = new QHBoxLayout(header_);
        headerLayout->setContentsMargins(5, 0, 5, 0);
        cardLayout->addWidget(header_);
        mascot_ = new QLabel(header_);
        pet_ = QPixmap(":/art/pug.png").scaled(29, 29, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        mascot_->setPixmap(pet_);
        headerLayout->addWidget(mascot_);
        clock_ = new QLabel(header_);
        headerLayout->addWidget(clock_);
        temperature_ = new QLabel("—°", header_);
        headerLayout->addWidget(temperature_);
        counter_ = new QLabel(header_);
        counter_->hide();
        headerLayout->addWidget(counter_);
        equalizer_ = new QLabel(QStringLiteral("▮▮▮"), header_);
        equalizer_->hide();
        headerLayout->addWidget(equalizer_);
        headerLayout->addStretch();
        panelButton_ = button(headerLayout, QStringLiteral("⌄"), [this] { expand(!open_); });
        panelButton_->setAccessibleName(tr("Раскрыть или свернуть Orbit"));

        tabs_ = new QTabWidget(card_);
        cardLayout->addWidget(tabs_);
        createPlayer();
        createWeather();
        createShelf();
        createNotes();
        createTimer();
        createReminders();
        addClipboardPage(tabs_, settings_);
        addToolsPage(tabs_);

        hover_.setSingleShot(true);
        hover_.setInterval(150);
        connect(&hover_, &QTimer::timeout, this, [this] { expand(true); });
        leave_.setSingleShot(true);
        leave_.setInterval(420);
        connect(&leave_, &QTimer::timeout, this, [this] { if (!underMouse()) expand(false); });
        clockTimer_.setSingleShot(true);
        connect(&clockTimer_, &QTimer::timeout, this, &Island::updateClock);
        alarm_.setSingleShot(true);
        alarm_.setTimerType(Qt::PreciseTimer);
        connect(&alarm_, &QTimer::timeout, this, &Island::finishTimer);
        weatherTimer_.setInterval(15 * 60 * 1000);
        weatherTimer_.setTimerType(Qt::VeryCoarseTimer);
        connect(&weatherTimer_, &QTimer::timeout, this, &Island::updateWeather);
        media_.changed = [this](const Track &track) { showTrack(track); };
        updateClock();
        expand(false);
        if (!testing) {
            createTray();
            restoreTimer();
            scheduleReminders();
            updateWeather();
            weatherTimer_.start();
            media_.refresh();
        }
    }

    bool smokeTest()
    {
        const bool compact = width() == 224 && tabs_->isHidden();
        expand(true);
        const bool expanded = width() > 224 && !tabs_->isHidden() && tabs_->count() == 8;
        expand(false);
        if (!compact || !expanded || width() != 224) return false;
        if (noteEditor_->toPlainText() != "Legacy note") return false;
        notes_.append(QVariantMap{{"name", QStringLiteral("Second note")}, {"text", QStringLiteral("Second body")}});
        saveNotes();
        selectNote(1);
        if (noteEditor_->toPlainText() != "Second body") return false;
        noteEditor_->setPlainText("Saved edit");
        selectNote(0);
        if (noteEditor_->toPlainText() != "Legacy note") return false;
        const auto saved = settings_.value("notes/items").toList();
        if (saved.size() != 2 || saved.at(1).toMap().value("text").toString() != "Saved edit") return false;
        startTimer(5);
        toggleTimerPause();
        if (alarm_.isActive() || !pausedRemaining_ || deadline_) return false;
        const auto paused = pausedRemaining_;
        restoreTimer();
        if (pausedRemaining_ != paused || alarm_.isActive()) return false;
        toggleTimerPause();
        if (!alarm_.isActive() || pausedRemaining_ || !deadline_) return false;
        stopTimer();
        restoreTimer();
        return !alarm_.isActive() && !deadline_ && !pausedRemaining_;
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        leave_.stop();
        if (!open_ && hoverEnabled_) hover_.start();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        hover_.stop();
        if (open_) leave_.start();
        QWidget::leaveEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    }

    void dropEvent(QDropEvent *event) override
    {
        for (const auto &url : event->mimeData()->urls()) {
            if (url.isLocalFile()) addFile(url.toLocalFile());
        }
        event->acceptProposedAction();
        expand(true);
        tabs_->setCurrentWidget(shelf_->parentWidget());
    }

private:
    QPushButton *button(QBoxLayout *layout, const QString &text, std::function<void()> action)
    {
        auto *widget = new QPushButton(text);
        connect(widget, &QPushButton::clicked, this, [action] { action(); });
        layout->addWidget(widget);
        return widget;
    }

    QVBoxLayout *page(const QString &title)
    {
        auto *widget = new QWidget(tabs_);
        tabs_->addTab(widget, title);
        auto *layout = new QVBoxLayout(widget);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(9);
        return layout;
    }

    void createPlayer()
    {
        auto *layout = page(tr("Главная"));
        auto *track = new QHBoxLayout;
        cover_ = new QLabel;
        cover_->setFixedSize(96, 96);
        cover_->setAlignment(Qt::AlignCenter);
        cover_->setPixmap(pet_);
        track->addWidget(cover_);
        auto *info = new QVBoxLayout;
        title_ = new QLabel(tr("Музыка рядом"));
        title_->setObjectName("title");
        title_->setWordWrap(true);
        title_->setTextFormat(Qt::PlainText);
#ifdef Q_OS_WIN
        artist_ = new QLabel(tr("Управление системными медиаклавишами"));
#else
        artist_ = new QLabel(tr("Откройте плеер с поддержкой MPRIS"));
#endif
        artist_->setWordWrap(true);
        artist_->setTextFormat(Qt::PlainText);
        info->addWidget(title_);
        info->addWidget(artist_);
        auto *transport = new QHBoxLayout;
        button(transport, QStringLiteral("⏮"), [this] { media_.send(Media::Previous); })->setAccessibleName(tr("Предыдущий трек"));
        button(transport, QStringLiteral("⏯"), [this] { media_.send(Media::PlayPause); })->setAccessibleName(tr("Пауза или воспроизведение"));
        button(transport, QStringLiteral("⏭"), [this] { media_.send(Media::Next); })->setAccessibleName(tr("Следующий трек"));
        transport->addStretch();
        info->addLayout(transport);
        track->addLayout(info, 1);
        layout->addLayout(track);
        auto *launcher = new QHBoxLayout;
        button(launcher, "Spotify", [] { QDesktopServices::openUrl(QUrl("https://open.spotify.com/")); });
        button(launcher, "ChatGPT", [] { QDesktopServices::openUrl(QUrl("https://chatgpt.com/")); });
        button(launcher, "Codex", [] { QDesktopServices::openUrl(QUrl("https://chatgpt.com/codex")); });
        layout->addLayout(launcher);
        layout->addStretch();
    }

    void showTrack(const Track &track)
    {
        title_->setText(track.title.isEmpty() ? tr("Музыка рядом") : track.title);
        artist_->setText(track.artist.isEmpty() ? tr("Системный плеер") : track.artist);
        equalizer_->setVisible(track.playing);
        if (track.cover == coverUrl_) return;
        coverUrl_ = track.cover;
        if (coverUrl_.isLocalFile()) {
            const QPixmap image(coverUrl_.toLocalFile());
            cover_->setPixmap(image.isNull() ? pet_ : image.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            cover_->setPixmap(pet_);
        }
    }

    void createWeather()
    {
        auto *layout = page(tr("Погода"));
        city_ = settings_.value("weather/city", QStringLiteral("Москва")).toString();
        weather_ = new QLabel(tr("Загрузка прогноза…"));
        weather_->setWordWrap(true);
        weather_->setTextFormat(Qt::PlainText);
        weather_->setObjectName("title");
        layout->addWidget(weather_);
        auto *forecast = new QTabWidget;
        hours_ = new QListWidget;
        week_ = new QListWidget;
        forecast->addTab(hours_, tr("Часы"));
        forecast->addTab(week_, tr("Неделя"));
        layout->addWidget(forecast, 1);
        auto *buttons = new QHBoxLayout;
        button(buttons, tr("Выбрать город"), [this] {
            bool ok = false;
            const QString city = QInputDialog::getText(this, tr("Погода"), tr("Город"), QLineEdit::Normal, city_, &ok).trimmed();
            if (ok && !city.isEmpty()) {
                city_ = city.left(80);
                settings_.setValue("weather/city", city_);
                updateWeather();
            }
        });
        button(buttons, tr("Обновить"), [this] { updateWeather(); });
        buttons->addStretch();
        layout->addLayout(buttons);
        layout->addWidget(new QLabel(tr("Данные Open-Meteo")));
    }

    void json(const QUrl &url, std::function<void(QJsonObject)> done)
    {
        if (!network_) network_ = new QNetworkAccessManager(this);
        QNetworkRequest request(url);
        request.setTransferTimeout(10000);
        auto *reply = network_->get(request);
        connect(reply, &QNetworkReply::finished, this, [reply, done] {
            const auto result = reply->error() == QNetworkReply::NoError
                ? QJsonDocument::fromJson(reply->readAll()).object() : QJsonObject{};
            reply->deleteLater();
            done(result);
        });
    }

    void updateWeather()
    {
        const auto request = ++weatherRequest_;
        QUrl geo("https://geocoding-api.open-meteo.com/v1/search");
        QUrlQuery placeQuery;
        placeQuery.setQueryItems({{"name", city_}, {"count", "1"}, {"language", "ru"}});
        geo.setQuery(placeQuery);
        json(geo, [this, request](const QJsonObject &response) {
            if (request != weatherRequest_) return;
            const auto places = response.value("results").toArray();
            if (places.isEmpty()) { weather_->setText(tr("Не удалось получить прогноз для %1").arg(city_)); return; }
            const auto place = places.at(0).toObject();
            QUrl forecast("https://api.open-meteo.com/v1/forecast");
            QUrlQuery forecastQuery;
            forecastQuery.setQueryItems({{"latitude", QString::number(place.value("latitude").toDouble())},
                {"longitude", QString::number(place.value("longitude").toDouble())},
                {"current", "temperature_2m,apparent_temperature,weather_code"},
                {"hourly", "temperature_2m,weather_code,precipitation"},
                {"daily", "temperature_2m_min,temperature_2m_max,weather_code"},
                {"timezone", "auto"}});
            forecast.setQuery(forecastQuery);
            json(forecast, [this, request, place](const QJsonObject &data) {
                if (request != weatherRequest_) return;
                const auto current = data.value("current").toObject();
                if (!current.value("temperature_2m").isDouble()) { weather_->setText(tr("Прогноз недоступен")); return; }
                const auto degrees = QString::number(qRound(current.value("temperature_2m").toDouble())) + QStringLiteral("°");
                const int code = current.value("weather_code").toInt();
                const QString symbol = weatherIcon(code);
                temperature_->setText(symbol + " " + degrees);
                weather_->setText(place.value("name").toString() + "\n" + symbol + " " + degrees +
                    tr(" · Ощущается как %1°").arg(qRound(current.value("apparent_temperature").toDouble())));
                showForecast(data, current.value("time").toString());
            });
        });
    }

    static QString weatherIcon(int code)
    {
        if (code <= 1) return QStringLiteral("☀");
        if (code <= 3) return QStringLiteral("☁");
        if (code == 45 || code == 48) return QStringLiteral("≋");
        if ((code >= 71 && code <= 77) || code == 85 || code == 86) return QStringLiteral("❄");
        return QStringLiteral("☂");
    }

    void showForecast(const QJsonObject &data, const QString &now)
    {
        hours_->clear();
        week_->clear();
        const auto hourly = data.value("hourly").toObject();
        const auto time = hourly.value("time").toArray();
        const auto degrees = hourly.value("temperature_2m").toArray();
        const auto weather = hourly.value("weather_code").toArray();
        const auto rain = hourly.value("precipitation").toArray();
        const int count = qMin(qMin(time.size(), degrees.size()), qMin(weather.size(), rain.size()));
        for (int i = 0; i < count && hours_->count() < 24; ++i) {
            const auto stamp = time.at(i).toString();
            if (stamp.left(13) < now.left(13)) continue;
            const auto amount = rain.at(i).toDouble();
            hours_->addItem(stamp.mid(11, 5) + "    " + weatherIcon(weather.at(i).toInt()) + "    " +
                            QString::number(qRound(degrees.at(i).toDouble())) + QStringLiteral("°") +
                            (amount > 0 ? tr("    %1 мм").arg(amount, 0, 'f', 1) : QString()));
        }
        const auto daily = data.value("daily").toObject();
        const auto days = daily.value("time").toArray();
        const auto low = daily.value("temperature_2m_min").toArray();
        const auto high = daily.value("temperature_2m_max").toArray();
        const auto codes = daily.value("weather_code").toArray();
        const int dayCount = qMin(qMin(days.size(), low.size()), qMin(high.size(), codes.size()));
        for (int i = 0; i < dayCount; ++i) {
            const auto date = QDate::fromString(days.at(i).toString(), Qt::ISODate);
            week_->addItem(date.toString("dd.MM") + "    " + weatherIcon(codes.at(i).toInt()) + "    " +
                           tr("%1° … %2°").arg(qRound(low.at(i).toDouble())).arg(qRound(high.at(i).toDouble())));
        }
    }

    void createShelf()
    {
        auto *layout = page(tr("Полка"));
        shelf_ = new QListWidget;
        shelf_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        layout->addWidget(shelf_);
        for (const auto &file : settings_.value("shelf/files").toStringList()) addFile(file, false);
        connect(shelf_, &QListWidget::itemDoubleClicked, this, [](QListWidgetItem *item) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(item->data(Qt::UserRole).toString()));
        });
        auto *buttons = new QHBoxLayout;
        button(buttons, tr("Добавить файлы"), [this] {
            for (const auto &file : QFileDialog::getOpenFileNames(this, tr("Добавить на полку"))) addFile(file);
        });
        button(buttons, tr("Убрать с полки"), [this] {
            const auto selected = shelf_->selectedItems();
            for (auto *item : selected) delete item;
            saveShelf();
        });
        layout->addLayout(buttons);
    }

    void addFile(const QString &file, bool save = true)
    {
        for (int i = 0; i < shelf_->count(); ++i) if (shelf_->item(i)->data(Qt::UserRole).toString() == file) return;
        auto *item = new QListWidgetItem(QUrl::fromLocalFile(file).fileName(), shelf_);
        item->setData(Qt::UserRole, file);
        item->setToolTip(file);
        if (save) saveShelf();
    }

    void saveShelf()
    {
        QStringList files;
        for (int i = 0; i < shelf_->count(); ++i) files.append(shelf_->item(i)->data(Qt::UserRole).toString());
        settings_.setValue("shelf/files", files);
    }

    void createNotes()
    {
        auto *layout = page(tr("Заметки"));
        auto *actions = new QHBoxLayout;
        notePicker_ = new QComboBox;
        actions->addWidget(notePicker_, 1);
        button(actions, tr("Новая"), [this] {
            bool ok = false;
            const auto name = QInputDialog::getText(this, tr("Новая заметка"), tr("Название"),
                                                    QLineEdit::Normal, tr("Моя заметка"), &ok).trimmed();
            if (!ok || name.isEmpty()) return;
            notes_.append(QVariantMap{{"name", name}, {"text", QString()}});
            saveNotes();
            selectNote(notes_.size() - 1);
        });
        button(actions, tr("Переименовать"), [this] {
            const int index = notePicker_->currentIndex();
            if (index < 0) return;
            auto note = notes_.at(index).toMap();
            bool ok = false;
            const auto name = QInputDialog::getText(this, tr("Переименовать заметку"), tr("Название"),
                                                    QLineEdit::Normal, note.value("name").toString(), &ok).trimmed();
            if (!ok || name.isEmpty()) return;
            note["name"] = name;
            notes_[index] = note;
            saveNotes();
            selectNote(index);
        });
        button(actions, tr("Удалить"), [this] {
            const int index = notePicker_->currentIndex();
            if (index < 0) return;
            if (QMessageBox::question(this, tr("Заметки"), tr("Удалить выбранную заметку?")) != QMessageBox::Yes) return;
            notes_.removeAt(index);
            saveNotes();
            selectNote(qMin(index, int(notes_.size()) - 1));
        });
        layout->addLayout(actions);

        noteEditor_ = new QTextEdit;
        noteEditor_->setAcceptRichText(false);
        noteEditor_->setPlaceholderText(tr("Заметка сохраняется автоматически"));
        layout->addWidget(noteEditor_);
        if (settings_.contains("notes/items")) notes_ = settings_.value("notes/items").toList();
        else notes_.append(QVariantMap{{"name", tr("Моя заметка")}, {"text", settings_.value("notes/text").toString()}});
        selectNote(qBound(-1, settings_.value("notes/selected", 0).toInt(), int(notes_.size()) - 1));
        connect(notePicker_, &QComboBox::currentIndexChanged, this, [this](int index) {
            showNote(index);
            settings_.setValue("notes/selected", index);
        });
        connect(noteEditor_, &QTextEdit::textChanged, this, [this] {
            const int index = notePicker_->currentIndex();
            if (index < 0) return;
            auto note = notes_.at(index).toMap();
            note["text"] = noteEditor_->toPlainText();
            notes_[index] = note;
            saveNotes();
        });
    }

    void saveNotes()
    {
        settings_.setValue("notes/items", notes_);
    }

    void showNote(int index)
    {
        const QSignalBlocker blocker(noteEditor_);
        noteEditor_->setEnabled(index >= 0);
        noteEditor_->setPlainText(index >= 0 ? notes_.at(index).toMap().value("text").toString() : QString());
    }

    void selectNote(int index)
    {
        const QSignalBlocker blocker(notePicker_);
        notePicker_->clear();
        for (const auto &entry : std::as_const(notes_)) notePicker_->addItem(entry.toMap().value("name").toString());
        notePicker_->setCurrentIndex(index);
        showNote(index);
        settings_.setValue("notes/selected", index);
    }

    void createReminders()
    {
        auto *layout = page(tr("Напоминания"));
        reminderPage_ = tabs_->count() - 1;
        reminders_ = new QListWidget;
        reminderMessage_ = new QLabel;
        reminderMessage_->setWordWrap(true);
        reminderMessage_->setTextFormat(Qt::PlainText);
        layout->addWidget(reminderMessage_);
        layout->addWidget(reminders_);
        auto *entry = new QHBoxLayout;
        auto *text = new QLineEdit;
        text->setPlaceholderText(tr("Текст напоминания"));
        text->setMaxLength(500);
        auto *when = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(600));
        when->setDisplayFormat("dd.MM.yyyy HH:mm");
        when->setCalendarPopup(true);
        auto *daily = new QCheckBox(tr("Ежедневно"));
        entry->addWidget(text, 1);
        entry->addWidget(when);
        entry->addWidget(daily);
        button(entry, tr("Добавить"), [this, text, when, daily] {
            if (text->text().trimmed().isEmpty() || when->dateTime() <= QDateTime::currentDateTime()) {
                reminderMessage_->setText(tr("Введите текст и выберите время в будущем"));
                return;
            }
            reminderEntries_.append(QVariantMap{{"text", text->text().trimmed()},
                {"at", when->dateTime().toMSecsSinceEpoch()}, {"daily", daily->isChecked()}});
            text->clear();
            reminderMessage_->clear();
            saveReminders();
        });
        layout->addLayout(entry);
        button(layout, tr("Удалить выбранное"), [this] {
            if (reminders_->currentRow() < 0) return;
            reminderEntries_.removeAt(reminders_->currentRow());
            saveReminders();
        });
        reminderEntries_ = settings_.value("reminders/items").toList();
        renderReminders();
        reminderTimer_.setSingleShot(true);
        connect(&reminderTimer_, &QTimer::timeout, this, &Island::checkReminders);
    }

    void renderReminders()
    {
        reminders_->clear();
        for (const auto &entry : std::as_const(reminderEntries_)) {
            const auto value = entry.toMap();
            const auto date = QDateTime::fromMSecsSinceEpoch(value.value("at").toLongLong());
            reminders_->addItem(date.toString("dd.MM HH:mm") + "  ·  " + value.value("text").toString() +
                                (value.value("daily").toBool() ? tr("  ·  Ежедневно") : QString()));
        }
    }

    void saveReminders()
    {
        settings_.setValue("reminders/items", reminderEntries_);
        renderReminders();
        scheduleReminders();
    }

    void scheduleReminders()
    {
        reminderTimer_.stop();
        if (reminderEntries_.isEmpty()) return;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        qint64 wait = 30000;
        for (const auto &entry : std::as_const(reminderEntries_)) {
            wait = qMin(wait, entry.toMap().value("at").toLongLong() - now);
        }
        reminderTimer_.start(int(qMax<qint64>(0, wait)));
    }

    void checkReminders()
    {
        const auto now = QDateTime::currentDateTime();
        QStringList due;
        for (int i = reminderEntries_.size() - 1; i >= 0; --i) {
            auto value = reminderEntries_[i].toMap();
            auto date = QDateTime::fromMSecsSinceEpoch(value.value("at").toLongLong());
            if (date > now) continue;
            due.prepend(value.value("text").toString());
            if (value.value("daily").toBool()) {
                do { date = date.addDays(1); } while (date <= now);
                value["at"] = date.toMSecsSinceEpoch();
                reminderEntries_[i] = value;
            } else {
                reminderEntries_.removeAt(i);
            }
        }
        if (due.isEmpty()) { scheduleReminders(); return; }
        const auto message = due.join("\n");
        reminderMessage_->setText(message);
        if (tray_ && QSystemTrayIcon::supportsMessages()) tray_->showMessage(tr("Напоминание"), message);
        expand(true);
        tabs_->setCurrentIndex(reminderPage_);
        saveReminders();
    }

    void createTimer()
    {
        auto *layout = page(tr("Таймер"));
        timerPage_ = tabs_->count() - 1;
        timerLabel_ = new QLabel(tr("Таймер не запущен"));
        timerLabel_->setObjectName("title");
        layout->addWidget(timerLabel_);
        auto *buttons = new QHBoxLayout;
        for (const auto minutes : {5, 15, 25}) {
            button(buttons, tr("%1 мин").arg(minutes), [this, minutes] { startTimer(minutes); });
        }
        button(buttons, tr("Своё время"), [this] {
            bool ok = false;
            const auto minutes = QInputDialog::getInt(this, tr("Таймер"), tr("Минуты"), 10, 1, 1440, 1, &ok);
            if (ok) startTimer(minutes);
        });
        timerPause_ = button(buttons, tr("Пауза"), [this] { toggleTimerPause(); });
        button(buttons, tr("Стоп"), [this] { stopTimer(); });
        layout->addLayout(buttons);
        layout->addStretch();
    }

    void startTimer(int minutes)
    {
        pausedRemaining_ = 0;
        const auto duration = minutes * 60000;
        deadline_ = QDateTime::currentMSecsSinceEpoch() + duration;
        alarm_.start(duration);
        saveTimer();
        updateClock();
    }

    void restoreTimer()
    {
        deadline_ = settings_.value("timer/end", 0).toLongLong();
        pausedRemaining_ = qBound<qint64>(0, settings_.value("timer/remaining", 0).toLongLong(), 86400000);
        if (deadline_ > 0 && !pausedRemaining_) {
            alarm_.start(int(qBound<qint64>(0, deadline_ - QDateTime::currentMSecsSinceEpoch(), 86400000)));
        }
        updateClock();
    }

    void saveTimer()
    {
        settings_.setValue("timer/end", deadline_);
        settings_.setValue("timer/remaining", pausedRemaining_);
    }

    void toggleTimerPause()
    {
        if (pausedRemaining_ > 0) {
            deadline_ = QDateTime::currentMSecsSinceEpoch() + pausedRemaining_;
            alarm_.start(int(pausedRemaining_));
            pausedRemaining_ = 0;
        } else if (deadline_ > 0) {
            const auto remaining = deadline_ - QDateTime::currentMSecsSinceEpoch();
            if (remaining <= 0) { finishTimer(); return; }
            pausedRemaining_ = remaining;
            deadline_ = 0;
            alarm_.stop();
        }
        saveTimer();
        updateClock();
    }

    void stopTimer()
    {
        alarm_.stop();
        deadline_ = 0;
        pausedRemaining_ = 0;
        saveTimer();
        updateClock();
    }

    void finishTimer()
    {
        stopTimer();
        if (tray_ && QSystemTrayIcon::supportsMessages()) tray_->showMessage("Orbit Native", tr("Время вышло"));
        expand(true);
        tabs_->setCurrentIndex(timerPage_);
        timerLabel_->setText(tr("Таймер завершён"));
    }

    void updateClock()
    {
        const auto now = QDateTime::currentDateTime();
        clock_->setText(now.time().toString("HH:mm"));
        const bool active = deadline_ > 0 || pausedRemaining_ > 0;
        counter_->setVisible(active);
        timerPause_->setEnabled(active);
        timerPause_->setText(pausedRemaining_ > 0 ? tr("Продолжить") : tr("Пауза"));
        if (active) {
            const auto remaining = pausedRemaining_ > 0 ? pausedRemaining_ : deadline_ - now.toMSecsSinceEpoch();
            const auto seconds = qMax<qint64>(0, (remaining + 999) / 1000);
            const auto label = QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
            counter_->setText(label);
            timerLabel_->setText(pausedRemaining_ > 0 ? label + tr(" · Пауза") : label);
        } else {
            timerLabel_->setText(tr("Таймер не запущен"));
        }
        const int period = deadline_ > 0 ? 1000 : 60000;
        clockTimer_.start(period - int(now.toMSecsSinceEpoch() % period) + 25);
    }

    void createTray()
    {
        if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
        tray_ = new QSystemTrayIcon(windowIcon(), this);
        tray_->setToolTip("Orbit Native");
        auto *menu = new QMenu(this);
        menu->addAction(tr("Открыть Orbit"), this, [this] { expand(true); });
        auto *hover = menu->addAction(tr("Раскрывать при наведении"));
        hover->setCheckable(true);
        hover->setChecked(hoverEnabled_);
        connect(hover, &QAction::toggled, this, [this](bool value) {
            hoverEnabled_ = value;
            settings_.setValue("island/hover", value);
        });
        menu->addAction(tr("Выход"), qApp, &QApplication::quit);
        tray_->setContextMenu(menu);
        tray_->show();
    }

    void expand(bool open)
    {
        open_ = open;
        hover_.stop();
        leave_.stop();
        tabs_->setVisible(open);
        panelButton_->setText(open ? QStringLiteral("⌃") : QStringLiteral("⌄"));
        const auto *screen = this->screen();
        const auto area = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
        setFixedSize(open ? qMin(760, area.width() - 20) : 224,
                     open ? qMin(386, area.height() - 30) : 44);
        move(area.center().x() - width() / 2, area.top() + 10);
    }

    QSettings settings_;
    Media media_{this};
    QTimer hover_, leave_, clockTimer_, alarm_, weatherTimer_, reminderTimer_;
    QNetworkAccessManager *network_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    QWidget *card_ = nullptr, *header_ = nullptr;
    QVBoxLayout *root_ = nullptr;
    QLabel *mascot_ = nullptr, *clock_ = nullptr, *temperature_ = nullptr, *counter_ = nullptr, *equalizer_ = nullptr;
    QLabel *title_ = nullptr, *artist_ = nullptr, *cover_ = nullptr, *weather_ = nullptr, *timerLabel_ = nullptr;
    QPushButton *panelButton_ = nullptr, *timerPause_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QListWidget *shelf_ = nullptr, *reminders_ = nullptr, *hours_ = nullptr, *week_ = nullptr;
    QLabel *reminderMessage_ = nullptr;
    QVariantList reminderEntries_;
    QComboBox *notePicker_ = nullptr;
    QTextEdit *noteEditor_ = nullptr;
    QVariantList notes_;
    QPixmap pet_;
    QUrl coverUrl_;
    QString city_;
    int weatherRequest_ = 0;
    int timerPage_ = 0;
    int reminderPage_ = 0;
    qint64 deadline_ = 0;
    qint64 pausedRemaining_ = 0;
    bool open_ = false;
    bool hoverEnabled_ = settings_.value("island/hover", true).toBool();
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName("OrbitNative");
    app.setApplicationName("Orbit Native");
    app.setApplicationVersion(ORBIT_VERSION);
    app.setStyle("Fusion");
    app.setStyleSheet(R"(
        QWidget { color: #eeecf6; font-size: 13px; }
        QWidget#island { background: #101014; border: 1px solid #25242e; border-radius: 19px; }
        QLabel#title { font-size: 21px; font-weight: 600; }
        QPushButton { background: #23222a; border: 0; border-radius: 8px; padding: 6px 10px; }
        QPushButton:hover, QTabBar::tab:selected { background: #383345; }
        QPushButton:focus { border: 1px solid #bba8ff; }
        QTabWidget::pane { background: #18171e; border: 1px solid #292832; border-radius: 12px; }
        QTabBar::tab { background: #201f26; padding: 7px 12px; margin-right: 3px; border-radius: 6px; }
        QListWidget, QTextEdit, QLineEdit, QSpinBox { background: #201f26; border: 1px solid #302d39; border-radius: 7px; padding: 5px; }
        QDialog, QMenu { background: #17161d; }
    )");
    const bool testing = app.arguments().contains("--smoke-test");
    std::unique_ptr<QTemporaryDir> testingProfile;
    if (testing) {
        testingProfile = std::make_unique<QTemporaryDir>();
        if (!testingProfile->isValid()) return 1;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, testingProfile->path());
        QSettings().setValue("notes/text", "Legacy note");
    }
    Island island(testing);
    if (testing) return island.smokeTest() ? 0 : 1;
    island.show();
    return app.exec();
}
