#include "storage/sleep_snapshot.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QBuffer>
#include <QImageWriter>
#include <QMap>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonDocument>
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const auto path = directory.path() + "/snapshot.png";
    kobalt::sleep::Snapshot snapshot;
    snapshot.image = QImage(127, 173, QImage::Format_RGB32);
    for (int y = 0; y < snapshot.image.height(); ++y)
        for (int x = 0; x < snapshot.image.width(); ++x)
            snapshot.image.setPixel(x, y, qRgb((x * 37 + y) % 256, (y * 17) % 256, (x + y * 31) % 256));
    snapshot.mode = kobalt::sleep::Mode::Cover;
    snapshot.orientation = Qt::InvertedLandscapeOrientation;
    snapshot.progress = QString::fromUtf8("42% lu · 読書");
    snapshot.remaining = "37 min";
    snapshot.dark = true;
    snapshot.dimPage = false;
    snapshot.percent = 42;
    QMap<int, qint64> sizes;
    for (int compression : {-1, 0, 1, 3, 9}) {
        QJsonObject measurement;
        require(kobalt::storage::measureSleepSnapshot(path, snapshot, "book-token", compression, measurement),
                "atomic measured write");
        const auto read = kobalt::storage::readSleepSnapshot(path, "book-token", snapshot.mode);
        require(read.image == snapshot.image && read.orientation == snapshot.orientation &&
                read.progress == snapshot.progress && read.remaining == snapshot.remaining &&
                read.dark == snapshot.dark && read.dimPage == snapshot.dimPage && read.percent == snapshot.percent,
                "lossless pixels and metadata");
        require(measurement.value("compression").toInt(-2) == compression && measurement.value("saved").toBool(),
                "requested codec and save outcome");
        require(measurement.value("supports_compression").isBool() &&
                measurement.value("supports_quality").isBool() &&
                measurement.value("codec_api").toString() == (compression < 0 ? "default" : "compression") &&
                measurement.value("codec_argument").toInt(-2) ==
                    (compression < 0 ? -1 : (compression * 91 + 8) / 9),
                "native codec capability and selected argument");
        sizes[compression] = QFile(path).size();
        require(measurement.value("bytes").toDouble() == QFile(path).size() &&
                measurement.value("encode_write_ms").isDouble() && measurement.value("commit_ms").isDouble() &&
                measurement.value("total_ms").toDouble() >= measurement.value("encode_write_ms").toDouble(),
                "measurement units and file size");
        require(kobalt::storage::readSleepSnapshot(path, "stale-token", snapshot.mode).image.isNull(),
                "stale-token rejection");
    }
    require(sizes[0] > sizes[1] && sizes[0] > sizes[-1], "compression changes output size");
    QMap<int, qint64> qualitySizes;
    for (int compression = 0; compression <= 9; ++compression) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        require(buffer.open(QIODevice::WriteOnly), "quality test buffer");
        QImageWriter writer(&buffer, "png");
        QJsonObject selected;
        require(kobalt::storage::configureSleepSnapshotCodec(writer, compression, false, true, selected),
                "Qt5 quality fallback selected");
        const int quality = selected.value("codec_argument").toInt(-1);
        require(selected.value("codec_api") == "quality" && writer.quality() == quality &&
                (100 - quality) * 9 / 91 == compression, "quality maps to requested zlib level");
        require(writer.write(snapshot.image), "quality fallback writes");
        require(QImage::fromData(bytes, "png") == snapshot.image, "quality fallback remains lossless");
        qualitySizes[compression] = bytes.size();
    }
    require(qualitySizes[0] > qualitySizes[1] && qualitySizes[0] > qualitySizes[9],
            "quality fallback changes output size");
    QByteArray unsupportedBytes;
    QBuffer unsupportedBuffer(&unsupportedBytes);
    QImageWriter unsupportedWriter(&unsupportedBuffer, "png");
    QJsonObject unsupported;
    require(!kobalt::storage::configureSleepSnapshotCodec(unsupportedWriter, 1, false, false, unsupported) &&
            unsupported.value("codec_api") == "unsupported", "unsupported controls fail explicitly");
    require(kobalt::storage::configureSleepSnapshotCodec(unsupportedWriter, -1, false, false, unsupported) &&
            unsupported.value("codec_api") == "default", "default needs no compression control");
    QJsonObject measurement;
    require(!kobalt::storage::measureSleepSnapshot(path, snapshot, "book-token", 10, measurement),
            "invalid compression rejected");
    require(kobalt::storage::readSleepSnapshot(path, "book-token", snapshot.mode).image == snapshot.image,
            "invalid experiment preserves prior file");
    require(kobalt::storage::writeSleepSnapshot(path, snapshot, "default-token"), "unchanged production writer");
    require(kobalt::storage::readSleepSnapshot(path, "default-token", snapshot.mode).image == snapshot.image,
            "production round trip");
    const auto coverPath = directory.path() + "/native-cover.png";
    QImage cover(64, 96, QImage::Format_ARGB32);
    cover.fill(Qt::transparent);
    cover.setPixel(32, 48, qRgb(255, 0, 0));
    require(cover.save(coverPath, "PNG"), "native cover fixture");
    const QFileInfo source(coverPath);
    snapshot.coverPath = source.absoluteFilePath();
    snapshot.coverBytes = source.size();
    snapshot.coverModified = source.lastModified().toMSecsSinceEpoch();
    QImage opaque(cover.size(), QImage::Format_RGB32);
    opaque.fill(Qt::white);
    QPainter painter(&opaque);
    painter.drawImage(0, 0, cover);
    painter.end();
    snapshot.image = kobalt::sleep::coverBackground(opaque, {127, 173});
    require(snapshot.image.pixelColor(63, 20) == Qt::white && snapshot.image.pixelColor(0, 20) == Qt::black,
            "transparent cover is white inside black letterbox");
    require(kobalt::storage::measureSleepPersistence(path, snapshot, "cover-token", 1, measurement),
            "native cover reference save");
    require(measurement.value("persistence_kind") == "reference" && !QFile::exists(path),
            "cover save writes metadata instead of another PNG");
    const auto loaded = kobalt::storage::readSleepSnapshot(path, "cover-token", snapshot.mode);
    require(loaded.image == snapshot.image && loaded.progress == snapshot.progress &&
            loaded.coverPath == snapshot.coverPath, "reference restores pixels and metadata");
    require(kobalt::storage::readSleepSnapshot(path, "stale", snapshot.mode).image.isNull(), "stale reference rejected");
    QFile reference(directory.path() + "/snapshot.json");
    require(reference.open(QIODevice::ReadOnly), "reference JSON");
    const auto original = QJsonDocument::fromJson(reference.readAll()).object();
    reference.close();
    const auto rewrite = [&](QJsonObject json) {
        require(reference.open(QIODevice::WriteOnly | QIODevice::Truncate), "rewrite reference fixture");
        reference.write(QJsonDocument(json).toJson(QJsonDocument::Compact));
        reference.close();
    };
    // A stale PNG must not defeat rejection of a damaged newer reference.
    require(kobalt::storage::measureSleepSnapshot(path, snapshot, "cover-token", 1, measurement), "older PNG fixture");
    for (const auto &field : {QStringLiteral("cover_bytes"), QStringLiteral("cover_modified")}) {
        for (double value : {-1.0, 0.5, 1e300}) {
            auto damaged = original;
            damaged[field] = value;
            rewrite(damaged);
            require(kobalt::storage::readSleepSnapshot(path, "cover-token", snapshot.mode).image.isNull(),
                    "malformed reference integer rejected without PNG fallback");
        }
    }
    auto future = original;
    future["version"] = 2;
    rewrite(future);
    require(kobalt::storage::readSleepSnapshot(path, "cover-token", snapshot.mode).image.isNull(),
            "future reference version rejected");
    rewrite(original);
    QFile changed(coverPath);
    require(changed.open(QIODevice::Append) && changed.write("changed") == 7, "change cover fixture");
    changed.close();
    require(kobalt::storage::readSleepSnapshot(path, "cover-token", snapshot.mode).image.isNull(),
            "changed source rejected");
    require(QFile::remove(coverPath), "remove native source");
    require(kobalt::storage::readSleepSnapshot(path, "cover-token", snapshot.mode).image.isNull(),
            "missing source rejected");
    require(kobalt::storage::measureSleepPersistence(path, snapshot, "fallback-token", 1, measurement) &&
            measurement.value("persistence_kind") == "png" && !reference.exists(), "missing source uses PNG fallback");
    require(kobalt::storage::readSleepSnapshot(path, "fallback-token", snapshot.mode).image == snapshot.image,
            "fallback keeps captured cover pixels");
    kobalt::storage::removeSleepSnapshot(path);
    require(!QFile::exists(path) && !reference.exists(), "both cache forms removed");
    require(directory.remove(), "scratch cleanup");
}
