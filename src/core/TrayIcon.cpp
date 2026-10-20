#include "TrayIcon.h"

#include <QColor>
#include <QDir>
#include <QPainter>
#include <QRectF>

QImage TrayIcon::render(int size, bool recording)
{
    // Geometry expressed as fractions of the canvas so it scales to any size
    // (hand-tuned at 64px: 6px inset, 16px radius, 9px dot).
    QImage img(size, size, QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);

    const qreal inset = size * (6.0 / 64.0);
    const qreal radius = size * (16.0 / 64.0);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(recording ? "#D93025" : "#1FA1AD"));
    p.drawRoundedRect(QRectF(inset, inset, size - 2 * inset, size - 2 * inset),
                      radius, radius);

    p.setBrush(QColor("#FFFFFF"));
    p.drawEllipse(QPointF(size / 2.0, size / 2.0),
                  size * (9.0 / 64.0), size * (9.0 / 64.0));
    p.end();

    return img;
}

bool TrayIcon::installToTheme(const QString &iconsRoot)
{
    // Panel size, general use, and HiDPI/window-icon; the SNI host downscales
    // from whichever bucket it picks.
    const int sizes[] = {24, 48, 256};
    const struct { const char *name; bool recording; } variants[] = {
        {kIdleName, false},
        {kRecordingName, true},
    };
    for (int s : sizes) {
        const QString dir = QStringLiteral("%1/hicolor/%2x%2/apps").arg(iconsRoot).arg(s);
        if (!QDir().mkpath(dir))
            return false;
        for (const auto &v : variants) {
            const QString path = dir + QStringLiteral("/%1.png").arg(QLatin1String(v.name));
            if (!render(s, v.recording).save(path, "PNG"))
                return false;
        }
    }
    return true;
}
