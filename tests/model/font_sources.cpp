#include "assets/assets.h"
#include "assets/font_catalog.h"
#include "assets/system_fonts.h"
#include "engine/crengine/reader_model.h"
#include "reader_inputs.h"
#include "support/qt_compat.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using namespace kobalt;
static void expect(bool value, const char *message) {
    if (!value)
        qFatal("%s", message);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    expect(argc == 3, "supply a bundled font fixture and a book");
    const QVector<ReadingFont> unordered{
        {"Zeta", {}, FontOrigin::Recommended, {}, {}},  {"Personal", {}, FontOrigin::Sideloaded, {}, {}},
        {"Alpha", {}, FontOrigin::Recommended, {}, {}}, {"Libron", {}, FontOrigin::Recommended, {}, {}},
        {"System", {}, FontOrigin::BuiltIn, {}, {}},    {"Mono", {}, FontOrigin::Interface, {}, {}}};
    const auto ordered = orderedReadingFonts(unordered);
    const QStringList expectedOrder{"Libron", "Alpha", "Zeta", "System", "Personal", "Mono"};
    for (int index = 0; index < ordered.size(); ++index)
        expect(ordered[index].family == expectedOrder[index],
               "font pickers share source groups and Libron-first ordering");
    QTemporaryDir temporary;
    expect(temporary.isValid(), "temporary directory");
    const auto root = temporary.path();
    for (const auto &directory : {"ebook-core", "system", "sideload"}) {
        expect(QDir().mkpath(root + '/' + directory), "font source directory");
        expect(QFile::copy(QString::fromLocal8Bit(argv[1]), root + '/' + directory + "/Regular.ttf"),
               "copy bundled font fixture");
    }
    const auto previous = QDir::currentPath();
    expect(QDir::setCurrent(root + "/system"), "select populated working directory");
    expect(discoverReadingFonts(FontSources{}).isEmpty(), "empty font sources must not scan working directory");
    expect(QDir::setCurrent(previous), "restore working directory");
    FontSources sources{{root + "/ebook-core/Regular.ttf"}, root + "/system", root + "/sideload"};
    const auto catalog = discoverReadingFonts(sources);
    expect(catalog.size() == 1, "duplicate families respect source precedence");
    expect(catalog.first().origin == FontOrigin::Recommended, "bundled reading font takes precedence");
    expect(catalog.first().files == sources.bundled, "lower-priority files do not mix with bundled family");
    sources.bundled.clear();
    auto system = discoverReadingFonts(sources);
    expect(system.size() == 1 && system.first().origin == FontOrigin::BuiltIn,
           "explicit system source takes precedence over sideload");
    QFile original(root + "/system/Regular.ttf");
    expect(original.open(QIODevice::ReadOnly), "read system font before wrapping");
    const auto wrapped = "QTD" + qCompress(original.readAll());
    original.close();
    expect(original.open(QIODevice::WriteOnly | QIODevice::Truncate) && original.write(wrapped) == wrapped.size(),
           "wrap system font as Kobo firmware does");
    original.close();
    kobalt::FontCopies copies;
    sources.systemFiles = systemFontFiles(sources.systemDirectory, copies);
    sources.systemDirectory.clear();
    system = discoverReadingFonts(sources);
    expect(system.size() == 1 && system.first().origin == FontOrigin::BuiltIn &&
               system.first().files == sources.systemFiles,
           "decoded firmware font remains built-in and takes precedence over sideload");
    const auto systemFamily = system.first().family;
    auto inputs = test::readerFonts();
    inputs.sources.systemFiles = sources.systemFiles;
    ReaderModel model(inputs);
    bool registered = false;
    for (const auto &font : model.fontCatalog()) {
        if (font.family == systemFamily) {
            registered = font.origin == FontOrigin::BuiltIn && font.files == sources.systemFiles;
        }
    }
    for (const auto *family : {monospaceFontFace, mathFontFace}) {
        bool previewable = false;
        for (const auto &font : model.fontCatalog())
            previewable |= font.family == family && !font.files.isEmpty();
        expect(previewable, "bundled fallback family has files for its font-name preview");
    }
    expect(registered, "engine registers the prepared font as built-in");
    QFile book(QString::fromLocal8Bit(argv[2]));
    expect(book.open(QIODevice::ReadOnly), "open book fixture");
    QString error;
    expect(model.open(book.readAll(), QSize(800, 1000), qt::object({{"font_face", systemFamily}}), &error),
           qPrintable(error));
    expect(model.fontFace() == systemFamily && !model.image(model.page()).isNull(),
           "prepared firmware font can be selected and render a book page");
    auto changed = sources;
    changed.systemFiles.clear();
    expect(!(changed == sources), "prepared font paths participate in source identity");
    sources.systemFiles.clear();
    auto sideload = discoverReadingFonts(sources);
    expect(sideload.size() == 1 && sideload.first().origin == FontOrigin::Sideloaded,
           "disabled system source leaves sideload");

    const auto nested = root + "/nested";
    QFile fixture(QString::fromLocal8Bit(argv[1]));
    expect(fixture.open(QIODevice::ReadOnly), "read font fixture for nested folders");
    const auto fontBytes = fixture.readAll();
    const auto place = [&](const QString &relative, const QByteArray &bytes) {
        const auto path = nested + '/' + relative;
        expect(QDir().mkpath(QFileInfo(path).absolutePath()), "create nested font folder");
        QFile file(path);
        expect(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write nested font");
        return path;
    };
    const QStringList found{place("Root.ttf", fontBytes), place("Family/Regular.otf", fontBytes),
                            place("Family/static/Bold.ttf", fontBytes)};
    place("Family/static/deeper/Italic.ttf", fontBytes);
    place(".hidden/Regular.ttf", fontBytes);
    place("Family/._Regular.otf", fontBytes);
    place("kobo/Encrypted.ttf", "QTE" + QByteArray(32, 'x'));
    place("kobo/Compressed.ttf", "QTD" + qCompress(fontBytes));
    expect(sideloadedFontFiles(nested) == found,
           "sideload scan reaches two folder levels and skips hidden and wrapped fonts");
    expect(QDir::setCurrent(nested), "select populated working directory");
    expect(sideloadedFontFiles({}).isEmpty(), "empty sideload source never scans working directory");
    expect(QDir::setCurrent(previous), "restore working directory");
    const auto nestedCatalog = discoverReadingFonts(FontSources{{}, {}, nested});
    expect(nestedCatalog.size() == 1 && nestedCatalog.first().origin == FontOrigin::Sideloaded &&
               nestedCatalog.first().files == found,
           "nested sideloaded files join one family");
}
