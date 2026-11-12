#include "workspace.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QPixmap>
#include <QScreen>
#include <QSettings>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

namespace {
QPushButton *addButton(QBoxLayout *layout, const QString &label)
{
    auto *button = new QPushButton(label);
    layout->addWidget(button);
    return button;
}
}

void addClipboardPage(QTabWidget *tabs, QSettings &settings)
{
    auto *page = new QWidget(tabs);
    tabs->addTab(page, QObject::tr("Буфер"));
    auto *layout = new QVBoxLayout(page);
    auto *enable = new QCheckBox(QObject::tr("Сохранять текст в истории буфера"), page);
    enable->setChecked(settings.value("clipboard/enabled", false).toBool());
    layout->addWidget(enable);
    auto *list = new QListWidget(page);
    list->setWordWrap(true);
    layout->addWidget(list);
    auto *actions = new QHBoxLayout;
    auto *copy = addButton(actions, QObject::tr("Скопировать"));
    auto *clear = addButton(actions, QObject::tr("Очистить"));
    layout->addLayout(actions);

    auto history = [&settings] { return settings.value("clipboard/history").toStringList(); };
    auto reload = [list, history] {
        list->clear();
        for (const auto &value : history()) {
            auto *item = new QListWidgetItem(value.simplified().left(170), list);
            item->setData(Qt::UserRole, value);
        }
    };
    reload();
    QObject::connect(enable, &QCheckBox::toggled, page, [&settings](bool enabled) {
        settings.setValue("clipboard/enabled", enabled);
    });
    QObject::connect(copy, &QPushButton::clicked, page, [list] {
        if (const auto *item = list->currentItem()) QApplication::clipboard()->setText(item->data(Qt::UserRole).toString());
    });
    QObject::connect(clear, &QPushButton::clicked, page, [&settings, reload] {
        settings.remove("clipboard/history");
        reload();
    });
    QObject::connect(QApplication::clipboard(), &QClipboard::dataChanged, page, [&settings, enable, history, reload] {
        if (!enable->isChecked()) return;
        const auto *mime = QApplication::clipboard()->mimeData();
        if (!mime || !mime->hasText()) return;
        for (const auto &format : mime->formats()) {
            if (format.contains("password", Qt::CaseInsensitive) ||
                format.contains("sensitive", Qt::CaseInsensitive) ||
                format.contains("ExcludeClipboardContentFromMonitorProcessing", Qt::CaseInsensitive) ||
                format.contains("CanIncludeInClipboardHistory", Qt::CaseInsensitive)) return;
        }
        const auto value = mime->text();
        if (value.trimmed().isEmpty() || value.size() > 16000) return;
        auto entries = history();
        if (!entries.isEmpty() && entries.first() == value) return;
        entries.removeAll(value);
        entries.prepend(value);
        while (entries.size() > 60) entries.removeLast();
        settings.setValue("clipboard/history", entries);
        reload();
    });
}

void addToolsPage(QTabWidget *tabs)
{
    auto *page = new QWidget(tabs);
    tabs->addTab(page, QObject::tr("Инструменты"));
    auto *layout = new QVBoxLayout(page);
    auto *text = new QTextEdit(page);
    text->setAcceptRichText(false);
    text->setPlaceholderText(QObject::tr("Вставьте текст для преобразования"));
    layout->addWidget(text);
    auto *actions = new QHBoxLayout;
    auto *upper = addButton(actions, QObject::tr("ВЕРХНИЙ"));
    auto *lower = addButton(actions, QObject::tr("нижний"));
    auto *clean = addButton(actions, QObject::tr("Убрать лишние пробелы"));
    auto *copy = addButton(actions, QObject::tr("Скопировать"));
    layout->addLayout(actions);
    auto *capture = addButton(layout, QObject::tr("Сохранить скриншот монитора"));
    auto *result = new QLabel(page);
    result->setWordWrap(true);
    layout->addWidget(result);
    QObject::connect(upper, &QPushButton::clicked, page, [text] { text->setPlainText(text->toPlainText().toUpper()); });
    QObject::connect(lower, &QPushButton::clicked, page, [text] { text->setPlainText(text->toPlainText().toLower()); });
    QObject::connect(clean, &QPushButton::clicked, page, [text] {
        auto lines = text->toPlainText().split('\n');
        for (auto &line : lines) line = line.simplified();
        text->setPlainText(lines.join('\n'));
    });
    QObject::connect(copy, &QPushButton::clicked, page, [text] { QApplication::clipboard()->setText(text->toPlainText()); });
    QObject::connect(capture, &QPushButton::clicked, page, [page, result] {
        if (QGuiApplication::platformName().startsWith("wayland")) {
            result->setText(QObject::tr("Для Wayland нужен портал захвата экрана. Используйте снимок системы."));
            return;
        }
        auto *window = page->window();
        window->hide();
        QTimer::singleShot(220, page, [window, result] {
            auto *screen = window->screen();
            const auto screenshot = screen ? screen->grabWindow(0) : QPixmap{};
            window->show();
            if (screenshot.isNull()) { result->setText(QObject::tr("Не удалось снять экран")); return; }
            const auto path = QFileDialog::getSaveFileName(window, QObject::tr("Сохранить скриншот"),
                                                           "Orbit.png", "PNG (*.png)");
            if (!path.isEmpty()) result->setText(screenshot.save(path, "PNG")
                ? QObject::tr("Скриншот сохранён") : QObject::tr("Не удалось сохранить скриншот"));
        });
    });
}
