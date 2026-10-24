#include "sleep_snapshot.h"
#include "support/qt_compat.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QSaveFile>

namespace kobalt::storage {
namespace {
constexpr qint64 maxPixels = 4 * 1024 * 1024;
constexpr qint64 maxBytes = 20 * 1024 * 1024;
bool validSize(QSize size) {
    return size.width() > 0 && size.height() > 0 && qint64(size.width()) * size.height() <= maxPixels;
}
bool validOrientation(int value) {
    return value == Qt::PortraitOrientation || value == Qt::LandscapeOrientation ||
           value == Qt::InvertedPortraitOrientation || value == Qt::InvertedLandscapeOrientation;
}
}
bool writeSleepSnapshot(const QString &path, const sleep::Snapshot &snapshot, const QString &token) {
    if (path.isEmpty() || token.isEmpty() || snapshot.mode == sleep::Mode::Stock ||
        !validSize(snapshot.image.size()) || !validOrientation(snapshot.orientation))
        return false;
    const auto metadata = qt::object({{"version", 1}, {"token", token}, {"mode", sleep::value(snapshot.mode)},
                               {"orientation", int(snapshot.orientation)}, {"progress", snapshot.progress},
                               {"remaining", snapshot.remaining}, {"dark", snapshot.dark}, {"dim", snapshot.dimPage},
                               {"percent", snapshot.percent}});
    const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    if (bytes.size() > 16384 || !QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QImageWriter writer(&file, "png");
    writer.setText("kobalt", QString::fromUtf8(bytes));
    return writer.write(snapshot.image) && file.size() <= maxBytes && file.commit();
}
sleep::Snapshot readSleepSnapshot(const QString &path, const QString &token, sleep::Mode mode) {
    if (token.isEmpty() || mode == sleep::Mode::Stock)
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxBytes)
        return {};
    QImageReader reader(&file, "png");
    reader.setDecideFormatFromContent(false);
    if (!validSize(reader.size()))
        return {};
    const auto text = reader.text("kobalt").toUtf8();
    if (text.size() > 16384)
        return {};
    const auto metadata = QJsonDocument::fromJson(text).object();
    const int orientation = metadata.value("orientation").toInt();
    if (metadata.value("version") != 1 || metadata.value("token") != token ||
        metadata.value("mode") != sleep::value(mode) || !validOrientation(orientation) ||
        !metadata.value("progress").isString() || !metadata.value("remaining").isString() ||
        !metadata.value("dark").isBool() || !metadata.value("dim").isBool())
        return {};
    auto image = reader.read();
    if (!validSize(image.size()))
        return {};
    // Older images have no percentage. Tiles that draw it leave it out.
    const int percent = metadata.value("percent").toInt(-1);
    return {std::move(image), Qt::ScreenOrientation(orientation), metadata.value("progress").toString(),
            metadata.value("remaining").toString(), metadata.value("dark").toBool(), mode, metadata.value("dim").toBool(),
            percent >= 0 && percent <= 100 ? percent : -1};
}
}
