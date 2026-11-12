#include "portal_screenshot.h"
#include "portal_request.h"

#include <QImageReader>
#include <QUrl>
#include <utility>

void portalScreenshot(QObject *parent, std::function<void(QImage, QString)> result)
{
    portalRequest(parent, "org.freedesktop.portal.Screenshot", "Screenshot", {QString()}, {{"interactive", false}},
        [result = std::move(result)](uint status, const QVariantMap &data) {
            if (status != 0) {
                result({}, status == 1 ? QStringLiteral("Снимок отменён") : QStringLiteral("Системный портал не смог сделать снимок экрана"));
                return;
            }
            const auto uri = QUrl(data.value("uri").toString());
            if (!uri.isLocalFile() || !uri.host().isEmpty()) {
                result({}, QStringLiteral("Портал не вернул файл снимка"));
                return;
            }
            QImageReader image(uri.toLocalFile());
            const auto size = image.size();
            if (!size.isValid() || qint64(size.width()) * size.height() > 40000000) {
                result({}, QStringLiteral("Размер снимка не поддерживается"));
                return;
            }
            auto screenshot = image.read();
            const auto error = screenshot.isNull() ? QStringLiteral("Не удалось открыть снимок") : QString();
            result(std::move(screenshot), error);
        });
}
