#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/private_text.h"
#include "interfaces/reader/ui/style/scale.h"
#include "interfaces/reader/ui/style/theme.h"
#include "interfaces/reader/views/reading_page.h"
#include "platforms/desktop/window.h"
#include "platforms/desktop/services.h"
#include "platforms/android/start_screen.h"
#include "platforms/android/canvas.h"
#include "platforms/android/reader.h"
#include "platforms/android/window_geometry.h"
#include <QStackedLayout>
#include "platforms/android/boox_frame_queue.h"
#include "interfaces/library/library_view.h"
#include "library/library_store.h"
#include "reading/reading_density.h"
#include "reading/appearance_settings.h"
#include "storage/reader_storage.h"
#include "storage/bookmark_store.h"
#include "storage/highlight_store.h"
#include "publication/zip_resource.h"
#include "build_info.h"
#include "support/book_fixtures.h"
#include "support/engine_fixtures.h"
#include "support/strings.h"
#include "engine/crengine/reader_model.h"
#include "interfaces/reader/ui/controls/page_scrubber.h"
#include "interfaces/reader/ui/controls/loading.h"
#include "interfaces/reader/settings/dictionaries_page.h"
#include "interfaces/reader/panels/dictionary_panel.h"
#include "interfaces/reader/panels/link_panel.h"
#include "interfaces/reader/panels/choice_screen.h"
#include "interfaces/reader/panels/bookmark_list.h"
#include "interfaces/reader/panels/search_panel.h"
#include "interfaces/reader/panels/chapter_graph.h"
#include "interfaces/reader/settings/header_footer_panel.h"
#include "interfaces/reader/settings/acknowledgments.h"
#include "interfaces/reader/ui/controls/paged_text.h"
#include <lvtinydom.h>

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QBuffer>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGraphicsView>
#include <QLabel>
#include <QLineEdit>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QPainter>
#include <QSaveFile>
#include <QSignalSpy>
#include <QSignalBlocker>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "engine/crengine/engine_activity.h"
#include "reading/engine_worker.h"
#include "support/qt_compat.h"
#include <QScopeGuard>
#include <QSemaphore>
#include <QThread>
#include <atomic>
#include <memory>

using namespace kobalt;

namespace {
AssetBundle *assets;
FontCopies *copies;
const QString book = QStringLiteral(KOBALT_TEST_BOOK);
QString captureDirectory;
QJsonArray captures;

class BlockedFile {
public:
    explicit BlockedFile(QString path) : path(std::move(path)), backup(this->path + ".saved") {
        moved = QFile::rename(this->path, backup);
        blocked = moved && QDir().mkdir(this->path);
    }
    ~BlockedFile() { restore(); }
    bool active() const { return blocked; }
    bool restore() {
        if (blocked && !QDir().rmdir(path))
            return false;
        blocked = false;
        if (moved && !QFile::rename(backup, path))
            return false;
        moved = false;
        return true;
    }
private:
    QString path, backup;
    bool moved = false, blocked = false;
};

using test::writeBook;

bool createBook(const QString &directory, const QString &filename = "text.epub") {
    // A small, offline EPUB with the same text used by the former desktop tests.
    const QString paragraph = "A reader keeps the chapter ready. Changing the size of the words should preserve "
                              "the passage. We can turn the page, visit another chapter, and return to the same place. ";
    QMap<QString, QByteArray> files;
    files["mimetype"] = "application/epub+zip";
    files["META-INF/container.xml"] =
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<rootfiles><rootfile full-path=\"book.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    QString manifest, spine, navigation;
    const bool illustrated = filename == "later-images.epub";
    const bool nonlinear = filename == "publisher-non-linear.epub";
    const bool vertical = filename == "formatting-unsupported-writing.epub";
    if (illustrated) {
        QImage image(64, 80, QImage::Format_RGB32);
        image.fill(Qt::red);
        QBuffer buffer(&files["image.png"]);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return false;
    }
    for (int chapter = 1; chapter <= 3; ++chapter) {
        const auto name = QString("c%1.xhtml").arg(chapter);
        QString body = QString("<h1>Chapter %1</h1>").arg(chapter);
        if (nonlinear && chapter == 2)
            body += "<p>Nonlinearspine</p>";
        if (illustrated && chapter == 2)
            body += "<p><img src=\"image.png\" alt=\"A red image\"/></p>";
        for (int index = 0; index < 30; ++index) {
            body += "<p>" + paragraph.repeated(3) + "</p>";
        }
        const QString style = vertical ? " style=\"writing-mode:vertical-lr\"" : "";
        files[name] = ("<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Chapter</title></head><body" + style + ">" +
                       body + "</body></html>").toUtf8();
        manifest += QString("<item id=\"c%1\" href=\"%2\" media-type=\"application/xhtml+xml\"/>").arg(chapter).arg(name);
        spine += QString("<itemref idref=\"c%1\"%2/>").arg(chapter).arg(nonlinear && chapter == 2 ? " linear=\"no\"" : "");
        navigation += QString("<navPoint id=\"n%1\" playOrder=\"%1\"><navLabel><text>Chapter %1</text>"
                              "</navLabel><content src=\"%2\"/></navPoint>").arg(chapter).arg(name);
    }
    if (illustrated)
        manifest += "<item id=\"image\" href=\"image.png\" media-type=\"image/png\"/>";
    files["book.opf"] =
        ("<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uid\">"
         "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"uid\">kobalt-fixture</dc:identifier>"
         "<dc:title>Native reader fixture</dc:title><dc:language>en</dc:language><dc:creator>Kobalt</dc:creator></metadata>"
         "<manifest>" + manifest + "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/></manifest>"
         "<spine toc=\"ncx\">" + spine + "</spine></package>").toUtf8();
    files["toc.ncx"] =
        ("<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><head>"
         "<meta name=\"dtb:uid\" content=\"kobalt-fixture\"/></head><docTitle><text>Native reader fixture</text>"
         "</docTitle><navMap>" + navigation + "</navMap></ncx>").toUtf8();
    return writeBook(directory, filename, files);
}

struct Fixture {
    QTemporaryDir directory{QDir::tempPath() + "/kobalt-profile-XXXXXX"};
    storage::Paths paths{directory.path()};
    bool prepared = false;
    std::atomic_int clockReads{0};
    const EngineFontInputs fontInputs;
    desktop::Window window;

    // `library` fills the profile with a synthetic library; `reading` marks one book in progress.
    explicit Fixture(bool dark = false, bool variableFont = false, bool library = false, bool reading = true,
                     bool annotationExports = false)
        : fontInputs(engineFontInputs(*assets, {}, variableFont ? directory.path() + "/fonts" : QString(), {})),
          window(prepare(dark, variableFont, library, reading), fontInputs, placeholderFontFamily(*assets, *copies),
                 [this] {
                     ++clockReads;
                     return QStringLiteral("09:00");
                 }, [this, annotationExports](ReaderHostServices &services) {
                     if (annotationExports)
                         services.annotationExportDirectory = [this] { return paths.directory + "/exports/annotations"; };
                 }) {
        window.resize(920, 800);
        window.show();
    }
    storage::Paths prepare(bool dark, bool variableFont, bool library, bool reading) {
        paths.stateReport = directory.path() + "/state.json";
        if (library)
            prepareLibrary(reading);
        prepared = directory.isValid() && storage::writeJson(paths.preferences(), {{"dark_mode", dark}, {"highlight_help_seen", true}});
        if (variableFont)
            prepared = prepared && QDir().mkpath(directory.path() + "/fonts") &&
                       QFile::copy(QStringLiteral(KOBALT_TEST_VARIABLE_FONT), directory.path() + "/fonts/Amstelvar.ttf");
        return paths;
    }
    // Invented books and authors exercise series, sorting and cover layouts.
    // The folder does not exist, so
    // the start-up check keeps these books instead of rescanning them.
    void prepareLibrary(bool reading) {
        struct Run {
            const char *series, *author;
            QStringList titles;
            QColor colour;
            double megabytes;
        };
        const QVector<Run> runs{
            {"The Neralis Cycle", "Aven Teral",
             {"Lanterns over Neralis", "The Glass Orchard", "A Map of Quiet Rivers", "The Seventh Weather",
              "When the Copper Moons Return"}, QColor("#1e3a5a"), 9},
            {"The Clockwork Marsh", "Mira Vennik",
             {"The Clockwork Marsh", "The Brass Heron", "An Invitation from the Tides", "The Silent Ferry",
              "Twelve Keys for Winter", "The Cartographer's Bell", "Where the Moss Remembers", "A Door beneath the Rain"},
             QColor("#c04a6a"), 1.2},
            {"The Amber Compass", "Oren Kelwick",
             {"North of the Amber Compass", "A Crown of Paper Birds", "The Travelling Harbour",
              "The Last Bridge to Velora", "The Compass Comes Home"}, QColor("#2a3a8a"), 0.8},
            {"Postcards from Tessel", "Sela Dornet",
             {"A Small Trouble in Tessel", "The Mayor's Missing Tuesday", "Tea with the Weather Bureau",
              "The Unlicensed Comet", "A Parcel for Yesterday", "The Museum of Almost Everything",
              "Three Unreasonable Umbrellas", "The Midnight Queue", "The Borrowed Lighthouse", "A Very Local Eclipse",
              "The Committee for Lost Socks", "Tessel at Closing Time"}, QColor("#3a6a2a"), 0.6},
            {"The Veylan Routes", "Jorin Astrel",
             {"Departure from Veylan", "The Empty Transit", "Signals through the Salt", "The Long Way to Oravel",
              "All Routes Lead to Morning"}, QColor("#5a3a1a"), 3},
            {"", "Aven Teral", {"The Orchard beyond the Weather", "A House for Unwritten Letters"}, QColor("#6a2a2a"), 2},
        };
        QDir().mkpath(directory.path() + "/library-covers");
        QJsonArray books;
        int number = 0;
        for (const auto &run : runs)
            for (int i = 0; i < run.titles.size(); ++i, ++number) {
                const auto key = QString("/kobalt-fixture-library/%1.epub").arg(number);
                const auto cover = QString("cover-%1.png").arg(number);
                QImage image(240, 360, QImage::Format_RGB32);
                image.fill(run.colour.lighter(100 + 12 * (i % 4)));
                QPainter painter(&image);
                painter.setPen(Qt::white);
                painter.setFont(QFont(ui::interfaceFont().family(), 22));
                painter.drawText(image.rect().adjusted(16, 16, -16, -16), Qt::AlignTop | Qt::TextWordWrap,
                                 run.titles[i]);
                painter.end();
                image.save(directory.path() + "/library-covers/" + cover);
                QJsonObject book{{"key", key},
                                 {"folder", "/kobalt-fixture-library"},
                                 {"name", run.titles[i] + ".epub"},
                                 {"title", run.titles[i]},
                                 {"author", run.author},
                                 {"cover", cover},
                                 {"size", run.megabytes * (1 + i % 3) * 1024 * 1024},
                                 {"added", double(1700000000000LL + number * 1000)},
                                 {"metadata", 1}};
                if (*run.series) {
                    book["series"] = run.series;
                    book["series_index"] = i + 1;
                }
                // Reading the first series: two finished, the third in progress.
                if (reading && number < 3) {
                    book["opened"] = double(1710000000000LL + number * 1000);
                    book["percent"] = number < 2 ? 100 : 38;
                    book["finished"] = number < 2;
                }
                books.append(book);
            }
        storage::writeJson(directory.path() + "/library.json",
                           QJsonObject{{"schema", 1},
                                       {"folders", QJsonArray{QJsonObject{{"key", "/kobalt-fixture-library"},
                                                                          {"label", "Books"}}}},
                                       {"books", books}});
    }
    ~Fixture() {
        window.close();
        if (!QTest::qWaitFor([this] { return window.idle(); }, 30000)) {
            QTest::qFail("Reader did not drain while closing", __FILE__, __LINE__);
        }
    }
    QJsonObject state() {
        if (auto *reader = window.readerWindow()) {
            reader->checkpoint();
        }
        return storage::readJson(paths.stateReport);
    }
    bool click(QWidget *target, QPoint position = QPoint(-1, -1)) {
        auto *reader = window.readerWindow();
        auto *canvas = window.findChild<QGraphicsView *>();
        if (!reader || !canvas || !target || !target->isVisible() || !target->isEnabled()) {
            return false;
        }
        if (position.x() < 0) {
            position = target->rect().center();
            // A taller typography panel can cover the page's center. Use
            // exposed paper to dismiss it, as a person would, and never send
            // a page tap to an unrelated control above it.
            if (target->objectName() == "rrPage") {
                for (int y : {position.y(), target->height() / 3, target->height() / 4}) {
                    const QPoint candidate(position.x(), y);
                    if (reader->childAt(target->mapTo(reader, candidate)) == target) {
                        position = candidate;
                        break;
                    }
                }
                if (reader->childAt(target->mapTo(reader, position)) != target)
                    return false;
            }
        }
        const auto point = canvas->mapFromScene(target->mapTo(reader, position));
        if (!canvas->viewport()->rect().contains(point)) {
            return false;
        }
        QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        return true;
    }
    QWidget *widget(const char *name) {
        auto *reader = window.readerWindow();
        auto *widget = reader ? reader->findChild<QWidget *>(name) : nullptr;
        return widget ? widget : window.findChild<QWidget *>(name);
    }
    bool press(const char *name) { return click(widget(name)); }
    bool pressAnnotationExport(const char *action = "rrAnnotationExportHtml") {
        if (!press("rrAnnotationMore")) return false;
        if (!QTest::qWaitFor([this, action] { return visible(action); })) return false;
        return press(action);
    }
    bool drag(QWidget *target, QPoint from, QPoint to, bool holdFirst = false) {
        auto *reader = window.readerWindow();
        auto *canvas = window.findChild<QGraphicsView *>();
        if (!reader || !canvas || !target)
            return false;
        const auto start = canvas->mapFromScene(target->mapTo(reader, from));
        const auto end = canvas->mapFromScene(target->mapTo(reader, to));
        if (!canvas->viewport()->rect().contains(start) || !canvas->viewport()->rect().contains(end))
            return false;
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, start);
        bool recognized = true;
        if (holdFirst)
            recognized = QTest::qWaitFor([this] { return state().value("annotation_dragging").toBool(); }, 2000);
        for (int i = 1; i <= 8; ++i) {
            QTest::mouseMove(canvas->viewport(), start + (end - start) * i / 8);
            QTest::qWait(5);
        }
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, end);
        return recognized;
    }
    bool selectWord(QWidget *target, QPoint position) {
        auto *reader = window.readerWindow();
        auto *canvas = window.findChild<QGraphicsView *>();
        auto *page = target && target->objectName() == "rrPage" ? static_cast<ReadingPage *>(target) : nullptr;
        if (!reader || !canvas || !page)
            return false;
        const auto at = canvas->mapFromScene(target->mapTo(reader, position));
        bool recognized = false;
        auto original = page->annotationDragStarted;
        page->annotationDragStarted = [&](QPoint point, int boundary) {
            recognized = true;
            return original(point, boundary);
        };
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        const bool ready = QTest::qWaitFor([&] { return recognized; }, 2000);
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        page->annotationDragStarted = original;
        return ready;
    }
    bool startAnnotation(QWidget *target, QPoint position) {
        if (visible("rrDictionary") && !press("rrDictionaryClose"))
            return false;
        if (visible("rrAnnotationPopover") && !press("rrAnnotationCancel"))
            return false;
        if (!press("rrPage") || !QTest::qWaitFor([this] { return visible("rrAnnotate"); }, 2000) ||
            !press("rrAnnotate") || !selectWord(target, position))
            return false;
        auto *paper = static_cast<ReadingPage *>(target);
        return QTest::qWaitFor([&] { return visible("rrAnnotationEdit") ||
            (!state().value("annotation_dragging").toBool() &&
             paper->annotationContains(position - QPoint(0, paper->topHeight()))); }, 2000) &&
            (visible("rrAnnotationEdit") || click(target, position));
    }
    bool hold(QWidget *target, QPoint position) {
        return selectWord(target, position) && QTest::qWaitFor([this] { return visible("rrSelectionLink"); }, 2000) &&
            press("rrSelectionLink");
    }
    bool visible(const char *name) {
        const auto *item = widget(name);
        return item && item->isVisible();
    }
    QString storedPath(const QString &filename) {
        if (annotationDatabaseEnabled())
            return paths.database();
        QDirIterator files(paths.annotations(), {filename}, QDir::Files, QDirIterator::Subdirectories);
        return files.hasNext() ? files.next() : QString();
    }
    QJsonArray stored(const QString &filename, const QString &key) {
        if (!annotationDatabaseEnabled())
            return storage::readJson(storedPath(filename)).value(key).toArray();
        const auto current = state();
        const auto identity = current.value("book").toString();
        const auto fingerprint = current.value("fingerprint").toString().toLatin1();
        QJsonArray rows;
        if (key == "highlights") {
            HighlightStore store(paths.annotations(), identity, fingerprint, "Fixture", paths.database());
            if (!store.load()) qFatal("Cannot read saved SQLite highlights");
            for (const auto &row : store.highlights()) rows.append(highlightJson(row));
        } else {
            BookmarkStore store(paths.annotations(), identity, fingerprint, "Fixture", paths.database());
            if (!store.load()) qFatal("Cannot read saved SQLite bookmarks");
            for (const auto &row : store.bookmarks()) rows.append(bookmarkJson(row));
        }
        return rows;
    }
    template<class Result, class Query>
    Result coordinateQuery(const QString &path, Result fallback, Query query) {
        auto *page = static_cast<ReadingPage *>(widget("rrPage"));
        if (!page)
            return fallback;
        const auto current = state();
        const auto size = page->image.size();
        const auto fonts = fontInputs;
        struct Pending {
            Result value;
            std::atomic_bool done{false};
        };
        auto pending = std::make_shared<Pending>();
        pending->value = fallback;
        // Even a separate model shares CREngine's global fonts and caches.
        // Keep its construction, queries and destruction on the reader's queue.
        qt::invoke(engineWorker(), [pending, path, current, size, fonts, query]() mutable {
            {
                EngineJob activity;
                QFile input(path);
                ReaderModel coordinates(fonts);
                QString error;
                if (input.open(QIODevice::ReadOnly) &&
                    coordinates.open(input.readAll(), size, current, &error) &&
                    coordinates.goTo(current.value("position").toString()))
                    pending->value = query(coordinates);
            }
            pending->done.store(true);
        }, Qt::QueuedConnection);
        if (!QTest::qWaitFor([pending] { return pending->done.load(); }, 5000)) {
            QTest::qFail("Coordinate probe did not finish on the engine worker", __FILE__, __LINE__);
            return fallback;
        }
        return pending->value;
    }
    QPoint selectionHandleTarget(QPoint point, bool start) {
        auto *page = static_cast<ReadingPage *>(widget("rrPage"));
        if (!page)
            return {-1, -1};
        const int top = page->topHeight();
        return coordinateQuery(book, QPoint(-1, -1), [point, start, top](ReaderModel &coordinates) {
            const auto token = coordinates.annotationTokenAt(point - QPoint(0, top));
            const auto geometry = coordinates.annotationGeometry(token.start, token.end);
            const auto edge = start ? geometry.start : geometry.end;
            return (edge.first + edge.last) / 2 - edge.outward * ui::px(4) + QPoint(0, top);
        });
    }
    AnnotationPageEdges pageEdges() {
        return coordinateQuery(book, AnnotationPageEdges{}, [](ReaderModel &coordinates) {
            return coordinates.annotationPageEdges();
        });
    }
    QPoint wordPoint(int ordinal = 0) {
        auto *page = static_cast<ReadingPage *>(widget("rrPage"));
        if (!page)
            return {-1, -1};
        const int top = page->topHeight();
        const auto size = page->image.size();
        return coordinateQuery(book, QPoint(-1, -1), [ordinal, top, size](ReaderModel &coordinates) mutable {
            QSet<QString> seen;
            for (int y = size.height() / 3; y < size.height() * 2 / 3; y += 12)
                for (int x = size.width() / 3; x < size.width() * 2 / 3; x += 12) {
                    const auto word = coordinates.wordAt({x, y});
                    if (!word.rectangles.isEmpty() && !seen.contains(word.start)) {
                        seen.insert(word.start);
                        if (ordinal-- == 0)
                            return word.rectangles.first().center() + QPoint(0, top);
                    }
                }
            return QPoint(-1, -1);
        });
    }
    QRect wordRect(const QString &text) {
        auto *page = static_cast<ReadingPage *>(widget("rrPage"));
        if (!page)
            return {};
        const int top = page->topHeight();
        const auto size = page->image.size();
        return coordinateQuery(book, QRect{}, [text, top, size](ReaderModel &coordinates) {
            for (int y = 0; y < size.height(); y += 12)
                for (int x = 0; x < size.width(); x += 16) {
                    const auto word = coordinates.wordAt({x, y});
                    if (word.text == text && !word.rectangles.isEmpty())
                        return word.rectangles.first().translated(0, top);
                }
            return QRect{};
        });
    }
    QPoint linkPoint(const QString &path, const QString &label) {
        auto *page = static_cast<ReadingPage *>(widget("rrPage"));
        if (!page)
            return {-1, -1};
        const int top = page->topHeight();
        return coordinateQuery(path, QPoint(-1, -1), [label, top](ReaderModel &coordinates) {
            for (const auto &link : coordinates.pageLinks())
                if (coordinates.linkDetails(link.source).label == label && !link.rectangles.isEmpty())
                    return link.rectangles.first().center() + QPoint(0, top);
            return QPoint(-1, -1);
        });
    }
};

void variants() {
    QTest::addColumn<int>("preset");
    QTest::addColumn<bool>("dark");
    for (int preset : {0, 1, 2}) {
        for (bool dark : {false, true}) {
            QTest::newRow(qPrintable(QString("%1-%2").arg(preset).arg(dark ? "dark" : "light"))) << preset << dark;
        }
    }
}

bool hasText(const QImage &source) {
    const auto image = source.convertToFormat(QImage::Format_ARGB32);
    int paper = 0, ink = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(row[x]) != 255) {
                continue;
            }
            paper += qGray(row[x]) > 180;
            ink += qGray(row[x]) < 80;
        }
    }
    return paper > 100 && ink > 100;
}

bool capture(Fixture &fixture, const QString &name) {
    // Model state and visibility can settle before queued layouts and proxy
    // cache updates. Deliver those updates before rendering the visible frame.
    QCoreApplication::processEvents();
    QString error;
    const auto path = fixture.window.saveScreenshot(captureDirectory, &error);
    const auto destination = captureDirectory + '/' + name + ".png";
    if (path.isEmpty() || !QFile::rename(path, destination)) {
        qCritical() << "Cannot save screenshot:" << name << error;
        return false;
    }
    const QImage image(destination);
    QFile file(destination);
    if (image.isNull() || !file.open(QIODevice::ReadOnly)) {
        return false;
    }
    captures.append(QJsonObject{{"file", name + ".png"}, {"width", image.width()}, {"height", image.height()},
                               {"sha256", QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex())}});
    return true;
}

void advanceIntoBody(Fixture &fixture) {
    // Capture narrative text as well as the chapter-opening typography.
    auto *reader = fixture.window.readerWindow();
    QVERIFY(reader);
    const int chapterStart = fixture.state().value("page").toInt();
    reader->pageButton(1);
    QTRY_VERIFY_WITH_TIMEOUT(fixture.state().value("page").toInt() > chapterStart, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
    auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
    QVERIFY(page && hasText(page->image));
}

void openPrologueBody(Fixture &fixture) {
    if (!fixture.visible("rrNavChapters"))
        QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrNavChapters"));
    QVERIFY(fixture.press("rrNavChapters"));
    QTRY_VERIFY(fixture.visible("rrChapter1"));
    QVERIFY(fixture.press("rrChapter1"));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
    advanceIntoBody(fixture);
}

bool revealSettingsOption(Fixture &fixture, const char *name) {
    auto *reader = fixture.window.readerWindow();
    auto *option = fixture.widget(name);
    if (!reader || !option)
        return false;
    for (int attempt = 0; attempt < 12; ++attempt) {
        auto *hit = reader->childAt(option->mapTo(reader, option->rect().center()));
        if (hit && (hit == option || option->isAncestorOf(hit)))
            return true;
        if (!fixture.press("rrSettingsOptionsNext"))
            return false;
    }
    return false;
}

// The same real-widget tour supplies integration checks and screenshot scenes.
void tour(Fixture &fixture, const QString &name, bool screenshots) {
    const auto scene = [&](const QString &suffix) { return !screenshots || capture(fixture, name + '-' + suffix); };
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrSettings"));
    QVERIFY(scene("menu"));
    QVERIFY(fixture.press("rrNavChapters"));
    QTRY_VERIFY(fixture.visible("rrContents"));
    QVERIFY(fixture.widget("rrChapter2"));
    QVERIFY(scene("chapters"));
    QVERIFY(fixture.press("rrChapter1"));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
    QVERIFY(fixture.state().value("page").toInt() > 0);
    advanceIntoBody(fixture);
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrNavText"));
    QVERIFY(fixture.press("rrNavStats"));
    QTRY_VERIFY(fixture.visible("rrChapterGraph"));
    QVERIFY(scene("stats"));
    QVERIFY(fixture.press("rrNavStats"));
    QTRY_VERIFY(!fixture.visible("rrChapterGraph"));
    QVERIFY(fixture.press("rrNavText"));
    QTRY_VERIFY(fixture.visible("rrFont"));
    QVERIFY(scene("text"));
    QVERIFY(fixture.press("rrFont"));
    QTRY_VERIFY(fixture.visible("rrFontChoice0"));
    QVERIFY(scene("fonts"));
    const auto originalTypography = fixture.state();
    const int originalSize = originalTypography.value("font_pixels").toInt();
    if (!screenshots)
        QVERIFY(fixture.press("rrFontChoice1"));
    QVERIFY(fixture.press("rrFontClose"));
    QTRY_VERIFY(!fixture.visible("rrChoices"));
    if (!screenshots) {
        QVERIFY(fixture.press("rrLarger"));
        QVERIFY(fixture.press("rrHeavier"));
        QVERIFY(fixture.press("rrLooser"));
        QVERIFY(fixture.press("rrAlignLeft"));
        QVERIFY(fixture.press("rrHyphenation_on"));
        QVERIFY(!fixture.visible("rrOpticalSize")); // The bundled faces are static.
    }
    QVERIFY(fixture.press("rrNavLayout"));
    QTRY_VERIFY(fixture.visible("rrHeaderFooter"));
    if (!screenshots)
        QVERIFY(fixture.press("rrWider"));
    QVERIFY(scene("layout"));
    QVERIFY(fixture.press("rrHeaderFooter"));
    QTRY_VERIFY(fixture.visible("rrHeaderFooterView"));
    QVERIFY(scene("header-footer"));
    if (!screenshots)
        QVERIFY(fixture.press("rrHeaderFooterPreset_alternative_a"));
    QVERIFY(fixture.press("rrHeaderFooterClose"));
    QTRY_VERIFY(!fixture.visible("rrHeaderFooterView"));
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
    QTRY_VERIFY(!fixture.visible("rrMenu"));
    if (!screenshots) {
        QTRY_VERIFY_WITH_TIMEOUT(fixture.state().value("font_pixels").toInt() > originalSize, 30000);
        QVERIFY(!fixture.state().value("font_face").toString().isEmpty());
        QVERIFY(fixture.state().value("weight_steps").toInt() > originalTypography.value("weight_steps").toInt());
        QVERIFY(fixture.state().value("line_spacing").toInt() > originalTypography.value("line_spacing").toInt());
        QVERIFY(fixture.state().value("margin_pixels").toInt() > originalTypography.value("margin_pixels").toInt());
        QCOMPARE(fixture.state().value("text_alignment").toString(), QString("left"));
        QCOMPARE(fixture.state().value("hyphenation").toString(), QString("on"));
        QCOMPARE(fixture.state().value("header_footer_preset").toString(), QString("alternative_a"));
    }
    auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
    QVERIFY(page);
    // Model idleness precedes the UI's appearance-apply continuation. The
    // toolbar disappears and Graph is enabled when reading accepts input.
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.visible("rrControlsTop") && fixture.widget("rrGraph")->isEnabled(), 30000);
    QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.state().value("bookmarked").toBool(), 3000);
    QVERIFY(scene("bookmarked"));
    if (screenshots) {
        // Creation dates are visible in the list. Prepare a fixed timestamp
        // on disk, then let the app load it normally before capturing it.
        QSignalSpy reopened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.readerWindow()->close();
        QTRY_VERIFY(fixture.window.idle());
        const auto current = fixture.state();
        BookmarkStore store(fixture.paths.annotations(), current.value("book").toString(),
                            current.value("fingerprint").toString().toLatin1(), "Fixture", fixture.paths.database());
        QVERIFY(store.load());
        auto entries = store.bookmarks();
        for (auto &entry : entries)
            entry.created = QDateTime::fromString("2000-01-01T12:00:00Z", Qt::ISODate).toUTC();
        store.setBookmarks(entries);
        QVERIFY(store.save());
        fixture.window.openBook(book);
        QTRY_COMPARE(reopened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_VERIFY(fixture.state().value("bookmarked").toBool());
        page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(page);
    }
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrBookmarks"));
    QVERIFY(fixture.press("rrBookmarks"));
    QTRY_VERIFY(fixture.visible("rrBookmark0"));
    QVERIFY(scene("bookmarks"));
    if (screenshots) {
        QVERIFY(fixture.press("rrBookmarkRemove0"));
        QTRY_VERIFY(fixture.visible("rrRemoveBookmark"));
        QVERIFY(scene("bookmark-remove"));
        QVERIFY(fixture.press("rrKeepBookmark"));
        QTRY_VERIFY(!fixture.visible("rrRemoveBookmark"));
    }
    QVERIFY(fixture.press("rrAnnotationsTab"));
    QTRY_VERIFY(fixture.visible("rrBookmarkListEmpty"));
    QVERIFY(fixture.widget("rrAnnotationsTab")->x() < fixture.widget("rrBookmarksTab")->x());
    for (const auto *name : {"rrBookmarksTab", "rrAnnotationsTab"}) {
        auto *heading = fixture.widget(name);
        QVERIFY(heading);
        QCOMPARE(heading->font().family(), ui::brandFont(ui::metrics::chrome::titleText).family());
        QCOMPARE(heading->font().pixelSize(), int(ui::metrics::chrome::titleText));
    }
    QVERIFY(scene("annotations"));
    if (!screenshots) {
        QVERIFY(fixture.widget("rrAnnotationMore")->isVisible());
        QVERIFY(!fixture.widget("rrAnnotationMore")->isEnabled());
    }
    QVERIFY(fixture.press("rrBookmarkListClose"));
    if (!fixture.visible("rrSearchOpen"))
        QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrSearchOpen"));
    QVERIFY(fixture.press("rrSearchOpen"));
    QTRY_VERIFY(fixture.visible("rrSearchQuery"));
    auto *query = fixture.window.readerWindow()->findChild<QLineEdit *>("rrSearchQuery");
    QVERIFY(query);
    query->setText("Trevelyan");
    QVERIFY(fixture.press("rrSearchSubmit"));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.visible("rrSearchResult0"), 30000);
    auto *status = fixture.window.readerWindow()->findChild<QLabel *>("rrSearchStatus");
    QVERIFY(status);
    QTRY_VERIFY_WITH_TIMEOUT(!status->text().contains("Searching", Qt::CaseInsensitive), 30000);
    QVERIFY(scene("search"));
    QVERIFY(fixture.press("rrSearchResult0"));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
    QTRY_VERIFY(!fixture.visible("rrSearch"));
    if (!fixture.visible("rrSettings"))
        QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrSettings"));
    QVERIFY(fixture.press("rrSettings"));
    QTRY_VERIFY(fixture.visible("rrSettingsPages"));
    QVERIFY(scene("settings"));
    const bool fill = fixture.window.findChild<QComboBox *>("desktopScreenPicker")->currentIndex() == 0;
    QCOMPARE(fixture.visible("rrSettingsDictionaries"), fill);
    QCOMPARE(fixture.visible("rrSettingsUI"), fill);
    if (fill) {
        QVERIFY(fixture.press("rrSettingsDictionaries"));
        if (screenshots) {
            // Disk space is a host input, like the clock. Keep it fixed in
            // the screenshot fixture while retaining the real catalogue UI.
            auto *dictionaries = dynamic_cast<reader::DictionariesPage *>(fixture.widget("rrDictionaries"));
            QVERIFY(dictionaries);
            dictionaries->setStorage({}, 32000000000LL);
        }
        QTRY_VERIFY(fixture.visible("rrDictionaryRow0"));
        QTRY_VERIFY(fixture.widget("rrDictionaryRow0")->width() == fixture.widget("rrDictionaryRow0")->parentWidget()->width());
        QVERIFY(fixture.widget("rrDictionaryRow0")->width() > 200);
        QVERIFY(scene("dictionaries"));
        QVERIFY(fixture.press("rrSettingsUI"));
        QVERIFY(scene("ui"));
        QVERIFY(fixture.press("rrSettingsSystem"));
        QVERIFY(revealSettingsOption(fixture, "rrDictionaryFontSize"));
        QVERIFY(scene("dictionary-ui"));
        QVERIFY(fixture.press("rrDictionaryFontSize"));
        QTRY_VERIFY(fixture.visible("rrDictionaryFontSizePopup"));
        QVERIFY(scene("dictionary-font-sizes"));
        QVERIFY(fixture.press("rrDictionaryFontSize100"));
        QTRY_VERIFY(!fixture.visible("rrDictionaryFontSizePopup"));
    }
    QVERIFY(fixture.press("rrSettingsAbout"));
    QTRY_VERIFY(fixture.visible("rrAboutVersion"));
    QVERIFY(scene("about"));
    QVERIFY(fixture.press("rrSettingsClose"));
    QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
    if (screenshots) {
        openPrologueBody(fixture);
        QVERIFY(fixture.press("rrPage"));
    }
    QTRY_VERIFY(fixture.visible("rrAnnotate"));
    QVERIFY(fixture.press("rrAnnotate"));
    QVERIFY(!fixture.visible("rrControlsTop"));
    QVERIFY(!fixture.visible("rrAnnotationHeader"));
    QVERIFY(fixture.press("rrPage"));
    QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
    QVERIFY(scene("annotating"));
    QVERIFY(fixture.press("rrPage"));
    if (screenshots) {
        const auto word = fixture.wordPoint();
        QVERIFY(word.x() >= 0);
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
        QVERIFY(scene("annotation-start"));
        QVERIFY(fixture.click(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationStyle_shaded"));
        QVERIFY(scene("annotation-actions"));
        QVERIFY(fixture.press("rrAnnotationStyle_shaded"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        // Persistence finishes before the engine delivers saved markup. Wait
        // for the yellow mark at the selected word before capturing the page.
        const auto savedTintPixels = [&] {
            const auto image = page->snapshot();
            const auto area = QRect(word - QPoint(80, 80), QSize(160, 160)).intersected(image.rect());
            int colored = 0;
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x) {
                    const auto pixel = image.pixelColor(x, y);
                    colored += pixel.red() > pixel.blue() + 5 && pixel.green() > pixel.blue() + 5;
                }
            return colored;
        };
        QTRY_VERIFY(savedTintPixels() > 0);
        QVERIFY(scene("annotation-saved"));
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationEdit"));
        QVERIFY(scene("annotation-edit"));
        QVERIFY(fixture.click(page, QPoint(10, page->height() / 2)));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
    }
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrAnnotate"));
    QVERIFY(fixture.press("rrAnnotate"));
    if (screenshots) {
        QTRY_VERIFY(!fixture.visible("rrAnnotationModeNotice"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrBookmarks"));
        QVERIFY(fixture.press("rrBookmarks"));
        QTRY_VERIFY(fixture.widget("rrAnnotationMore")->isVisible() &&
                    fixture.widget("rrAnnotationMore")->isEnabled());
        QVERIFY(fixture.widget("rrAnnotationMore")->x() > fixture.widget("rrMarksSort")->x());
        QVERIFY(scene("annotation-export-list"));
        QVERIFY(fixture.press("rrAnnotationMore"));
        QTRY_VERIFY(fixture.visible("rrAnnotationExportHtml"));
        QVERIFY(scene("annotation-export-menu"));
        auto *exportAction = qobject_cast<QPushButton *>(fixture.widget("rrAnnotationExportHtml"));
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        QVERIFY(exportAction && canvas);
        const auto exportPoint = canvas->mapFromScene(exportAction->mapTo(fixture.window.readerWindow(), exportAction->rect().center()));
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, exportPoint);
        QTRY_VERIFY(exportAction->isDown());
        QVERIFY(scene("annotation-export-pressed"));
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, exportPoint);
        QTRY_VERIFY(fixture.visible("rrAnnotationExportConfirm"));
        QVERIFY(scene("annotation-export-confirmation"));
        QVERIFY(fixture.press("rrAnnotationExportConfirm"));
        QTRY_VERIFY(fixture.visible("rrAnnotationExportDone"));
        QVERIFY(scene("annotation-export-saved"));
        QVERIFY(fixture.press("rrAnnotationExportDone"));
    }
}
}

class DesktopTests final : public QObject {
    Q_OBJECT
private slots:
    void diagnostics() {
        // Capture native stdout/stderr too: Qt's message handler does not see
        // libpng errors or CREngine's direct printf diagnostics.
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {"startup", "libraryOnboarding", "reading:0-light", "errors", "features"});
        // Leave time to kill and reap the child before the CMake parent's limit.
        const bool finished = child.waitForFinished(20000);
        if (!finished) {
            child.kill();
            child.waitForFinished();
        }
        const auto output = child.readAll();
        QVERIFY2(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, output.constData());
        QVERIFY2(!output.contains("libpng error:"), output.constData());
        QVERIFY2(!output.contains("CRE WARNING:"), output.constData());
        QVERIFY2(!output.contains("styles re-init needed"), output.constData());
    }
    void storageDiagnostics() {
        // ldomDocument unregisters its fonts on destruction, even when empty.
        struct Fonts {
            bool owned = !fontMan;
            ~Fonts() { if (owned) ShutdownFontManager(); }
        } fonts;
        if (fonts.owned)
            QVERIFY(InitFontManager(lString8()));
        class Storage final : public ldomDataStorageManager {
        public:
            Storage(ldomDocument &document, lUInt32 limit)
                : ldomDataStorageManager(&document, 't', limit, 65536) {}
            bool warned() const { return _maxSizeReachedWarned; }
        };
        ldomDocument document;
        Storage normal(document, 1024 * 1024);
        const auto address = normal.allocText(16, 0, lString8("A stored paragraph"));
        normal.compact(0xFFFFFF);
        QVERIFY(!normal.warned());
        QCOMPARE(QByteArray(normal.getText(address).c_str()), QByteArray("A stored paragraph"));
        // Actual pressure still warns, even after a deliberate compaction.
        Storage limited(document, 1);
        limited.compact(0xFFFFFF);
        QVERIFY(!limited.warned());
        limited.allocText(16, 0, lString8("A stored paragraph"));
        QVERIFY(limited.getUncompressedSize() > 1);
        limited.compact(0);
        QVERIFY(limited.warned());
    }
    void startup() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        QCOMPARE(fixture.window.findChild<QComboBox *>("desktopScreenPicker")->currentIndex(), 1);
        QCOMPARE(fixture.window.findChild<QComboBox *>("desktopResolutionPicker")->currentData().toInt(), 0);
        QVERIFY(!fixture.window.readerWindow());
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start && start->libraryShown());
        QVERIFY(start->library()->findChild<QPushButton *>("libraryOpenBook")->isVisible());
        QVERIFY(!start->findChild<QWidget *>("androidIntro"));
    }
    void libraryOnboarding() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        library::Store store(directory.path(), std::make_shared<library::LocalSource>());
        android::StartScreen start(desktop::readerServices().presentation, store);
        start.resize(640, 800);
        start.show();
        QVERIFY(start.libraryShown());
        QVERIFY(!start.dismissModal());
        auto *view = start.library();
        QVERIFY(!view->findChild<QPushButton *>("libraryBack"));
        QVERIFY(!view->findChild<QWidget *>("librarySort")->isVisible());
        QVERIFY(!view->findChild<QPushButton *>("libraryViewGrid")->isVisible());
        QVERIFY(!view->findChild<QPushButton *>("libraryLeave"));
        QSignalSpy open(&start, &android::StartScreen::libraryOpenRequested);
        view->findChild<QPushButton *>("libraryOpenBook")->click();
        QTRY_COMPARE(open.count(), 1);
        QVERIFY(start.libraryShown());
        QVERIFY(QDir().mkpath(directory.path() + "/books"));
        QVERIFY(QFile::copy(book, directory.path() + "/books/first.epub"));
        connect(&start, &android::StartScreen::addFolderRequested, &start, [&] {
            store.addFolder({directory.path() + "/books", "Books"});
        });
        view->findChild<QPushButton *>("libraryChooseFolder")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!store.index().folders.isEmpty() && !store.scanning(), 10000);
        QCOMPARE(store.index().visibleCount(), 1);
        QVERIFY(!view->findChild<QWidget *>("libraryEmpty")->isVisible());
        QVERIFY(view->findChild<QPushButton *>("librarySort")->isVisible());
        QVERIFY(start.libraryShown());
    }
    void library() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start);
        const auto libraryBooks = [&fixture] {
            library::Index index;
            QJsonArray books;
            if (index.loadDatabase(fixture.directory.path()))
                for (const auto &book : index.books) books.append(book.json());
            return books;
        };
        QVERIFY(start->libraryShown());
        QTRY_COMPARE(start->library()->size(), QSize(1072, 1448));
        QVERIFY(start->library()->findChild<QWidget *>("libraryEmpty")->isVisible());
        // Menu, Finder and dropped paths use this same entry point.
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        fixture.window.readerWindow()->close();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
        QVERIFY(start->libraryShown());
        QTRY_VERIFY_WITH_TIMEOUT(libraryBooks().size() == 1, 5000);
        QVERIFY(libraryBooks().first().toObject().value("opened").toDouble() > 0);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        fixture.window.readerWindow()->close();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
        QCOMPARE(libraryBooks().size(), 1);
        // Its cover is saved and the cover grid draws it. The library itself and
        // the drawing for a missing cover are gray; the fixture's cover is tan.
        const auto cover = libraryBooks().first().toObject().value("cover").toString();
        QVERIFY(!cover.isEmpty());
        QVERIFY(!QImage(fixture.directory.path() + "/library-covers/" + cover).isNull());
        const auto pages = start->library()->findChild<QWidget *>("libraryPages")->grab().toImage().convertToFormat(
            QImage::Format_RGB32);
        int coloured = 0;
        for (int y = 0; y < pages.height(); y += 4)
            for (int x = 0; x < pages.width(); x += 4) {
                const QRgb pixel = pages.pixel(x, y);
                coloured += qAbs(qRed(pixel) - qBlue(pixel)) > 12;
            }
        QVERIFY2(coloured > 200, "The cover grid did not draw the book's cover");

        // Library is the root, not a page above an intro screen.
        QVERIFY(!start->dismissModal());
        QVERIFY(start->libraryShown());
    }
    void libraryTextInheritance() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto original = fixture.state();
        openPrologueBody(fixture);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavText"));
        QVERIFY(fixture.press("rrFont"));
        QTRY_VERIFY(fixture.visible("rrFontChoice0"));
        QVERIFY(fixture.press("rrFontChoice0"));
        QVERIFY(fixture.press("rrFontClose"));
        QTRY_VERIFY(fixture.state().value("preview_ready").toBool() && !fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrLarger"));
        QTRY_VERIFY(fixture.state().value("preview_ready").toBool() && !fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrLooser"));
        QTRY_VERIFY(!fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrAlignJustify"));
        QTRY_VERIFY(!fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.visible("rrMenu") && !fixture.state().value("busy").toBool());
        const auto first = fixture.state();
        QVERIFY(first.value("font_pixels").toInt() > original.value("font_pixels").toInt());
        const auto other = fixture.directory.path() + "/unopened-book.epub";
        QVERIFY(QFile::copy(book, other));
        fixture.window.openBook(other);
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto inherited = fixture.state();
        for (const auto *key : {"font_face", "font_pixels", "line_spacing", "text_alignment"})
            QCOMPARE(inherited.value(key), first.value(key));
        fixture.window.readerWindow()->close();
        QTRY_VERIFY(fixture.window.idle());

        // A previously read book has both a position and a distinct override.
        const auto identity = first.value("book").toString();
        auto saved = storage::readBookState(fixture.paths, identity).object;
        QVERIFY(!saved.value("position").toString().isEmpty());
        saved["font_pixels"] = first.value("font_pixels").toInt() + 12;
        QVERIFY(storage::writeBookState(fixture.paths, identity, saved));
        const auto fingerprint = saved.value("fingerprint").toString().toLatin1();
        BookmarkStore bookmarks(fixture.paths.annotations(), identity, fingerprint, "Reader fixture", fixture.paths.database());
        bookmarks.setBookmarks({{saved.value("position").toString(), "Prologue", "Saved bookmark",
                                 QDateTime::currentDateTimeUtc()}});
        QVERIFY(bookmarks.save());
        HighlightStore highlights(fixture.paths.annotations(), identity, fingerprint, "Reader fixture", fixture.paths.database());
        highlights.setHighlights({{"kept-highlight", original.value("position").toString(),
                                  saved.value("position").toString(), "Saved highlight", "Prologue",
                                  QDateTime::currentDateTimeUtc()}});
        QVERIFY(highlights.save());
        const auto preferences = storage::readJson(fixture.paths.preferences());
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start && start->libraryShown());
        start->library()->markBook(book, library::Book::Mark::Unread);
        QCOMPARE(storage::readBookState(fixture.paths, identity).object, QJsonObject({{"book", identity}}));
        QCOMPARE(storage::readJson(fixture.paths.preferences()), preferences);
        QVERIFY(bookmarks.load() && bookmarks.bookmarks().size() == 1);
        QVERIFY(highlights.load() && highlights.highlights().size() == 1);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 3);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto restarted = fixture.state();
        QCOMPARE(restarted.value("page").toInt(), 0);
        for (const auto *key : {"font_face", "font_pixels", "line_spacing", "text_alignment"})
            QCOMPARE(restarted.value(key), first.value(key));
    }
    void libraryGridRows() {
        // 37 books. Medium covers make 4 by 3 on both previews, four pages.
        // On the 7-inch, Small makes 5 by 4 (two pages) and Large 3 by 2 (seven).
        Fixture fixture(false, false, true, true);
        QVERIFY(fixture.prepared);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        auto *picker = fixture.window.findChild<QComboBox *>("desktopScreenPicker");
        QVERIFY(start && picker);
        start->showLibrary();
        auto *view = start->library();
        auto *canvas = start->libraryCanvas();
        auto *counter = view->findChild<QLabel *>("libraryPage");
        QVERIFY(counter);
        const auto pages = [](int count) {
            return strings::get("navigation.page_count").arg(1).arg(count);
        };
        for (const int preset : {1, 2}) {
            picker->setCurrentIndex(preset);
            QTRY_VERIFY(view->isVisible() && view->size() == canvas->sceneRect().size().toSize() &&
                        view->width() == (preset == 1 ? 1072 : 1264));
            QTRY_COMPARE(counter->text(), pages(4));
        }
        const auto chooseCover = [view](const char *name) {
            view->findChild<QPushButton *>("librarySort")->click();
            auto *choice = view->findChild<QAbstractButton *>(name);
            QVERIFY(choice && choice->isVisible());
            choice->click();
        };
        chooseCover("libraryCoverSmall");
        QTRY_COMPARE(counter->text(), pages(2));
        chooseCover("libraryCoverLarge");
        QTRY_COMPARE(counter->text(), pages(7));
    }
    void libraryHold() {
        // Holding a book opens its menu in every view, through the canvas as
        // the Mac app delivers the press.
        Fixture fixture(false, false, true, true);
        QVERIFY(fixture.prepared);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start);
        start->showLibrary();
        auto *view = start->library();
        auto *canvas = start->libraryCanvas();
        QTRY_VERIFY(view->isVisible() && view->size() == canvas->sceneRect().size().toSize() && view->width() > 600);
        auto *pages = view->findChild<QWidget *>("libraryPages");
        QVERIFY(pages);
        for (const char *mode : {"libraryViewGrid", "libraryViewList", "libraryViewHome"}) {
            view->findChild<QPushButton *>(mode)->click();
            // The first book: the top left of the pages, inside its cover or row.
            const QPoint inPages(pages->width() / 8, pages->height() / 8);
            const QPoint point = canvas->mapFromScene(QPointF(pages->mapTo(view, inPages)));
            QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            // A resting finger drifts a few points; the hold must survive that.
            const QPoint drift = point + QPoint(4, 3);
            QMouseEvent move(QEvent::MouseMove, drift, canvas->viewport()->mapToGlobal(drift), Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            QTRY_VERIFY_WITH_TIMEOUT(view->findChild<QWidget *>("libraryBookMenu"), 2000);
            QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, drift);
            QVERIFY(view->dismiss());
            QTRY_VERIFY(!view->findChild<QWidget *>("libraryBookMenu"));
        }
        // Switching views closes the sort choices.
        view->findChild<QPushButton *>("libraryViewGrid")->click();
        view->findChild<QPushButton *>("librarySort")->click();
        auto *sheet = view->findChild<QWidget *>("librarySortSheet");
        QVERIFY(sheet && sheet->isVisible());
        view->findChild<QPushButton *>("libraryViewList")->click();
        QVERIFY(!sheet->isVisible());
    }
    void libraryAtStart() {
        // Once there is a library, a fresh start opens on it.
        Fixture fixture(false, false, true, false);
        QVERIFY(fixture.prepared);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start && start->libraryShown());
        QVERIFY(!start->dismissModal());
        QVERIFY(start->libraryShown());
    }
    void reading_data() { variants(); }
    void reading() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        auto *reader = fixture.window.readerWindow();
        QVERIFY(reader);
        QVERIFY(fixture.clockReads.load() > 0);
        QCOMPARE(reader->property("reader_dark").toBool(), dark);
        QVERIFY(fixture.state().value("pages").toInt() > 2);
        openPrologueBody(fixture);
        QVERIFY(hasText(reader->findChild<QWidget *>("rrPage")->grab().toImage()));
        const int page = fixture.state().value("page").toInt();
        reader->pageButton(1);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.state().value("page").toInt() > page, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        const auto position = fixture.state().value("position").toString();
        QVERIFY(!position.isEmpty());
        QVERIFY(fixture.click(reader->findChild<QWidget *>("rrPage")));
        auto *exit = reader->findChild<QPushButton *>("rrReturn");
        QTRY_VERIFY(exit->isVisible());
        QVERIFY(fixture.click(exit));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        QCOMPARE(fixture.state().value("position").toString(), position);
    }
    void errors() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        QSignalSpy failed(&fixture.window, &desktop::Window::bookFailed);
        fixture.window.openBook(fixture.directory.path() + "/missing.epub");
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
        fixture.window.openBook(book);
        fixture.window.close();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
        QVERIFY(!fixture.window.isVisible());
    }
    void features() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        tour(fixture, {}, false);
    }
    void bookmarks_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void bookmarks() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(page);
        QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
        const auto original = fixture.stored("bookmarks.json", "bookmarks");
        QCOMPARE(original.size(), 1);
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrBookmarks"));
        QVERIFY(fixture.press("rrBookmarks"));
        QTRY_VERIFY(fixture.visible("rrBookmarkRemove0"));
        QVERIFY(!fixture.widget("rrAnnotationMore")->isVisible());
        QVERIFY(fixture.press("rrBookmarkRemove0"));
        QTRY_VERIFY(fixture.visible("rrRemoveBookmark"));
        QCOMPARE(fixture.stored("bookmarks.json", "bookmarks"), original);
        QVERIFY(fixture.press("rrKeepBookmark"));
        QTRY_VERIFY(!fixture.visible("rrRemoveBookmark"));
        QCOMPARE(fixture.stored("bookmarks.json", "bookmarks"), original);
        QVERIFY(fixture.press("rrBookmarkRemove0"));
        QTRY_VERIFY(fixture.visible("rrConfirmRemoveBookmark"));
        QVERIFY(fixture.press("rrConfirmRemoveBookmark"));
        QTRY_VERIFY(!fixture.visible("rrRemoveBookmark"));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 0);
        QVERIFY(fixture.stored("bookmarks.json", "bookmarks").isEmpty());
        QVERIFY(!page->bookmarked);
        QVERIFY(fixture.visible("rrBookmarkListEmpty"));
        QVERIFY(fixture.press("rrBookmarkListClose"));
        if (!fixture.visible("rrReturn"))
            QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrReturn"));
        // Closing the list intentionally guards Exit against the release
        // of the tap that dismissed it. Wait for that defined input interval.
        QTest::qWait(ui::metrics::contents::exitGuardMilliseconds + 20);
        QVERIFY(fixture.press("rrReturn"));
        QTRY_VERIFY(fixture.window.idle());
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("bookmark_count").toInt(), 0);
    }
    void annotations_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("pickerExport");
        QTest::addColumn<bool>("markdownExport");
        QTest::newRow("light-default-html") << false << false << false;
        QTest::newRow("dark-default-markdown") << true << false << true;
        QTest::newRow("light-picker-markdown") << false << true << true;
        QTest::newRow("dark-picker-html") << true << true << false;
    }
    void bookmarkMerge() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(page);
        for (int index = 0; index < 2; ++index) {
            QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
            QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), index + 1);
            if (!index) {
                const int before = fixture.state().value("page").toInt();
                fixture.window.readerWindow()->pageButton(1);
                QTRY_VERIFY(!fixture.state().value("busy").toBool() && fixture.state().value("page").toInt() > before);
            }
        }
        const int originalSize = fixture.state().value("font_pixels").toInt();
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrNavText"));
        QVERIFY(fixture.press("rrNavText"));
        QTRY_VERIFY(fixture.visible("rrTextSize"));
        auto *size = fixture.widget("rrTextSize");
        QVERIFY(fixture.click(size, {0, size->height() / 2}));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_VERIFY(fixture.state().value("font_pixels").toInt() < originalSize);
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
        QCOMPARE(fixture.stored("bookmarks.json", "bookmarks").size(), 1);
        QTRY_VERIFY(fixture.state().value("bookmarked").toBool());
        QTRY_VERIFY(!fixture.visible("rrControlsTop") && fixture.widget("rrGraph")->isEnabled());
        QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 0);
        QVERIFY(fixture.stored("bookmarks.json", "bookmarks").isEmpty());
    }
    void touchCursor() {
        Fixture fixture(false);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        QVERIFY(canvas);
        const QPoint point = canvas->viewport()->rect().center();
        QMouseEvent hoverEvent(QEvent::MouseMove, point, canvas->viewport()->mapToGlobal(point), Qt::NoButton,
                               Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &hoverEvent);
        QVERIFY(QApplication::overrideCursor());
        const auto hover = QApplication::overrideCursor()->pixmap().toImage();
        QVERIFY(!hover.isNull());
        QCOMPARE(QApplication::overrideCursor()->hotSpot(), QPoint(14, 14));
        QCOMPARE(hover.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(hover.pixelColor(hover.width() / 2, hover.height() / 2).alpha(), 128);
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        const auto pressed = QApplication::overrideCursor()->pixmap().toImage();
        QVERIFY(pressed.pixelColor(pressed.width() / 2, pressed.height() / 2).alpha() >
                hover.pixelColor(hover.width() / 2, hover.height() / 2).alpha());
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(QApplication::overrideCursor()->pixmap().toImage(), hover);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&fixture.window, &leave);
        QVERIFY(!QApplication::overrideCursor());
        auto *screens = fixture.window.findChild<QComboBox *>("desktopScreenPicker");
        screens->setCurrentIndex(0);
        QTest::mouseMove(canvas->viewport(), point + QPoint(1, 0));
        QVERIFY(!QApplication::overrideCursor());
    }
    void touchCursorOutsideReader() {
        // E-reader presets use the touch pointer across the whole window,
        // including the preview toolbar and the space around the device.
        Fixture fixture(false);
        QVERIFY(fixture.prepared);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        auto *picker = fixture.window.findChild<QComboBox *>("desktopScreenPicker");
        QVERIFY(start && picker);
        const auto hover = [](QWidget *target) {
            const QPoint point = target->rect().center();
            QMouseEvent move(QEvent::MouseMove, point, target->mapToGlobal(point), Qt::NoButton, Qt::NoButton,
                             Qt::NoModifier);
            QApplication::sendEvent(target, &move);
        };
        const auto touchPointer = [] {
            const auto *cursor = QApplication::overrideCursor();
            return cursor && !cursor->pixmap().isNull() && cursor->hotSpot() == QPoint(14, 14);
        };
        hover(start);
        QVERIFY(touchPointer());
        hover(picker);
        QVERIFY(touchPointer());
        start->showLibrary();
        auto *library = start->libraryCanvas();
        QTRY_VERIFY(library->isVisible());
        hover(library->viewport());
        QVERIFY(touchPointer());
        hover(picker);
        QVERIFY(touchPointer());
        hover(&fixture.window);
        QVERIFY(touchPointer());
        // The seven-inch preset follows the same rule.
        picker->setCurrentIndex(2);
        hover(picker);
        QVERIFY(touchPointer());
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&fixture.window, &leave);
        QVERIFY(!touchPointer());
        hover(picker);
        QVERIFY(touchPointer());
        QEvent deactivate(QEvent::WindowDeactivate);
        QApplication::sendEvent(&fixture.window, &deactivate);
        QVERIFY(!touchPointer());
        // Fill window keeps the normal pointer.
        picker->setCurrentIndex(0);
        hover(library->viewport());
        QVERIFY(!touchPointer());
    }
    void libraryPageButtons() {
        Fixture fixture(false, false, true);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(2);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start && start->libraryShown());
        auto *library = start->library();
        QTRY_VERIFY(library->isVisible() && library->size() == QSize(1264, 1680));
        QApplication::setActiveWindow(&fixture.window);
        for (const auto *mode : {"libraryViewGrid", "libraryViewList"}) {
            library->findChild<QPushButton *>(mode)->click();
            auto *counter = library->findChild<QLabel *>("libraryPage");
            QVERIFY(counter && counter->isVisible());
            const auto initial = counter->text();
            fixture.window.setFocus();
            QTest::keyClick(&fixture.window, Qt::Key_PageDown);
            QTRY_VERIFY(counter->text() != initial);
            QTest::keyClick(&fixture.window, Qt::Key_PageUp);
            QTRY_COMPARE(counter->text(), initial);
            auto *next = fixture.window.findChild<QPushButton *>("desktopDeviceNext");
            auto *previous = fixture.window.findChild<QPushButton *>("desktopDevicePrevious");
            QVERIFY(next && previous);
            QTRY_VERIFY(next->isVisible() && previous->isVisible() && next->isEnabled() && previous->isEnabled());
            QTest::mouseClick(next, Qt::LeftButton);
            QTRY_VERIFY(counter->text() != initial);
            QTest::mouseClick(previous, Qt::LeftButton);
            QTRY_COMPARE(counter->text(), initial);
            library->findChild<QPushButton *>("libraryMore")->click();
            library->pageButton(1);
            QCOMPARE(counter->text(), initial);
            QVERIFY(library->dismiss());
            library->showNotice("Notice", "Hardware buttons must not page behind this notice.");
            library->pageButton(1);
            QCOMPARE(counter->text(), initial);
            QVERIFY(library->dismiss());
        }
    }
    void hardwarePageButtons_data() {
        QTest::addColumn<QString>("view");
        for (const auto *view : {"dictionary", "dictionary-choices", "footnote", "settings", "settings-fonts",
                                "reading-fonts", "stats", "fields", "chapters", "bookmarks", "search",
                                "acknowledgments", "license", "downloaded-dictionaries", "blocked-modal"})
            QTest::newRow(view) << QString::fromLatin1(view);
    }
    void hardwarePageButtons() {
        QFETCH(QString, view);
        Fixture fixture;
        QVERIFY(fixture.prepared);
        if (view == "downloaded-dictionaries")
            fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(0);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCoreApplication::processEvents();
        auto *reader = fixture.window.readerWindow();
        const auto position = fixture.state().value("position");
        const QString definition = QString("<p>A long definition with enough text to wrap and fill several pages.</p>").repeated(100);
        const char *counterName = nullptr;
        if (view.startsWith("dictionary")) {
            auto *panel = dynamic_cast<DictionaryPanel *>(fixture.widget("rrDictionary"));
            QVERIFY(panel);
            QVector<DictionaryChoice> choices;
            for (int i = 0; i < 35; ++i)
                choices.append({QString::number(i), QString("Dictionary %1").arg(i)});
            panel->setChoices(choices);
            DictionaryResult result;
            result.received = result.restored = true;
            result.dictionary = "Dictionary 0";
            result.dictionaryId = "0";
            result.html = definition;
            panel->open("word", result, {100, 100, 50, 40});
            counterName = "rrDictionaryPages";
            if (view == "dictionary-choices") {
                QVERIFY(fixture.press("rrDictionarySelect"));
                counterName = "rrDictionaryChoicesPages";
            }
        } else if (view == "footnote") {
            auto *panel = dynamic_cast<LinkPanel *>(fixture.widget("rrLinkPanel"));
            QVERIFY(panel);
            LinkDetails details;
            details.kind = LinkKind::Footnote;
            details.html = definition;
            details.target = "passage";
            panel->open(details, {100, 100, 50, 40});
            counterName = "rrLinkPages";
        } else if (view == "settings" || view == "settings-fonts" ||
                   view == "downloaded-dictionaries" || view == "blocked-modal") {
            QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrSettings"));
            QVERIFY(fixture.press(view == "downloaded-dictionaries" ? "rrSettingsDictionaries" : "rrSettingsSystem"));
            counterName = view == "downloaded-dictionaries" ? "rrDictionariesPage" : "rrSettingsOptionsPage";
            if (view == "settings-fonts") {
                QVERIFY(revealSettingsOption(fixture, "rrUiFont"));
                QVERIFY(fixture.press("rrUiFont"));
                counterName = "rrSettingsFontPage";
            }
        } else if (view == "reading-fonts") {
            QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrNavText"));
            QVERIFY(fixture.press("rrFont"));
            auto *choices = dynamic_cast<ChoiceScreen *>(fixture.widget("rrChoices"));
            QVERIFY(choices);
            QVector<Choice> fonts;
            for (int i = 0; i < 40; ++i)
                fonts.append({QString("Font %1").arg(i), "Libron", QString("testFont%1").arg(i)});
            choices->showChoices("Font", fonts, {}, ChoiceScreen::Mode::Fonts);
            counterName = "rrFontPagerPage";
        } else if (view == "stats") {
            auto *graph = dynamic_cast<ChapterGraph *>(fixture.widget("rrChapterGraph"));
            QVERIFY(graph);
            QVector<ChapterLink> chapters;
            for (int i = 0; i < 50; ++i)
                chapters.append({QString("Chapter %1").arg(i), QString::number(i), i, 0, 100});
            graph->changed = {};
            graph->setDense(false);
            graph->setData(chapters, {}, true, {});
            graph->open();
            counterName = "rrGraphPage";
        } else if (view == "fields") {
            auto *fields = dynamic_cast<kobalt::reader::FieldChooser *>(fixture.widget("rrHeaderFooterChooser"));
            QVERIFY(fields);
            fields->setGeometry(reader->rect());
            fields->open(0, headerfooter::choosableFields[0], {}, false);
            fields->show();
            fields->raise();
            counterName = "rrHeaderFooterFieldsPage";
        } else if (view == "chapters") {
            auto *choices = dynamic_cast<ChoiceScreen *>(fixture.widget("rrChoices"));
            QVERIFY(choices);
            QVector<Choice> chapters;
            for (int i = 0; i < 50; ++i)
                chapters.append({QString("Chapter %1").arg(i), QString::number(i), QString("testChapter%1").arg(i)});
            choices->changed = {};
            choices->showChoices("Chapters", chapters);
            counterName = "rrContentsPage";
        } else if (view == "bookmarks") {
            auto *list = dynamic_cast<BookmarkList *>(fixture.widget("rrBookmarkList"));
            QVERIFY(list);
            QVector<BookmarkRow> rows;
            for (int i = 0; i < 40; ++i)
                rows.append({QString::number(i), "Chapter", "Excerpt", {}, i});
            list->showBookmarks(rows);
            counterName = "rrBookmarkListPage";
        } else if (view == "search") {
            auto *search = dynamic_cast<SearchPanel *>(fixture.widget("rrSearch"));
            QVERIFY(search);
            search->open(false, 0);
            SearchBatch batch;
            batch.done = true;
            for (int i = 0; i < 40; ++i)
                batch.hits.append({QString::number(i), QString::number(i + 1), "Before", "match", "after", i});
            search->append(batch, 100);
            counterName = "rrSearchPage";
        } else {
            auto *acknowledgments = dynamic_cast<Acknowledgments *>(fixture.widget("rrAcknowledgments"));
            QVERIFY(acknowledgments);
            acknowledgments->setGeometry(reader->rect());
            acknowledgments->open();
            if (view == "license") {
                QVERIFY(fixture.press("rrAcknowledgment0"));
                for (auto *child : acknowledgments->findChildren<QWidget *>())
                    if (auto *license = dynamic_cast<LicenseText *>(child))
                        license->setText(QString("A long license paragraph.\n\n").repeated(300));
                acknowledgments->resize(acknowledgments->width(), acknowledgments->height() - 1);
            }
            counterName = "rrAcknowledgmentsPage";
        }
        // Dictionary rows redraw after their nested layout has resized.
        QCoreApplication::processEvents();
        auto *counter = reader->findChild<QLabel *>(counterName);
        QVERIFY(counter && counter->isVisible());
        auto *pager = dynamic_cast<ui::PagerBar *>(counter->parentWidget());
        QVERIFY(pager && pager->next->isEnabled());
        const auto initial = counter->text();
        if (view == "blocked-modal") {
            ui::NoticeModal notice(reader, {"", "Notice", {}}, {"OK", "testNoticeClose"});
            notice.show();
            notice.raise();
            reader->pageButton(1);
            QCOMPARE(counter->text(), initial);
            QCOMPARE(fixture.state().value("position"), position);
            notice.hide();
        }
        reader->pageButton(1);
        QTRY_VERIFY(counter->text() != initial);
        QCOMPARE(fixture.state().value("position"), position);
        reader->pageButton(-1);
        QTRY_COMPARE(counter->text(), initial);
        QApplication::setActiveWindow(&fixture.window);
        fixture.window.setFocus();
        QTest::keyClick(&fixture.window, Qt::Key_PageDown);
        QTRY_VERIFY(counter->text() != initial);
        QTest::keyClick(&fixture.window, Qt::Key_PageUp);
        QTRY_COMPARE(counter->text(), initial);
        QCOMPARE(fixture.state().value("position"), position);
    }
    void dictionaryDismissal_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("action");
        const QStringList actions{"left", "centre", "right", "close", "hide-reader"};
        for (bool dark : {false, true})
            for (int action = 0; action < actions.size(); ++action)
                QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light", actions[action]))) << dark << action;
    }
    void dictionaryDismissal() {
        QFETCH(bool, dark);
        QFETCH(int, action);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        auto *reader = fixture.window.readerWindow();
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        const auto word = fixture.wordPoint();
        QVERIFY(word.x() >= 0);
        const auto position = fixture.state().value("position");
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QVERIFY(page->annotationContains(word - QPoint(0, page->topHeight())));
        QVERIFY(!qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        android::takeRefreshFrame(reader, canvas->viewport(), QRegion(page->rect()));
        if (action == 4) {
            auto *dictionary = fixture.widget("rrDictionary");
            reader->hide();
            const bool dismissedWhileHidden = dictionary->isHidden();
            reader->show();
            QVERIFY(dismissedWhileHidden);
            QTRY_VERIFY(!fixture.visible("rrDictionary"));
            QCOMPARE(fixture.state().value("position"), position);
            return;
        }
        if (action == 3) {
            QVERIFY(fixture.press("rrDictionaryClose"));
        } else {
            auto *dictionary = fixture.widget("rrDictionary");
            const int y = dictionary->y() > 0 ? dictionary->y() - ui::px(24)
                                             : dictionary->geometry().bottom() + ui::px(24);
            const auto point = page->mapFrom(reader, QPoint((1 + 4 * action) * reader->width() / 10, y));
            QVERIFY(!page->annotationContains(point - QPoint(0, page->topHeight())));
            QCOMPARE(reader->childAt(page->mapTo(reader, point)), page);
            QVERIFY(fixture.click(page, point));
        }
        QTRY_VERIFY(!fixture.visible("rrDictionary"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(!fixture.visible("rrControlsTop"));
        QTRY_VERIFY(!page->annotationContains(word - QPoint(0, page->topHeight())));
        QVERIFY(page->annotationHandleHitRegion().isEmpty());
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(!qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        const QRegion damage(page->rect());
        QVERIFY(android::takeRefreshFrame(reader, canvas->viewport(), damage).clean);
        QVERIFY(!android::takeRefreshFrame(reader, canvas->viewport(), damage).clean);
    }
    void wordSelection_data() { annotationSelection_data(); }
    void wordSelection() {
        QFETCH(bool, dark);
        QFETCH(int, resolution);
        Fixture fixture(dark);
        auto *picker = fixture.window.findChild<QComboBox *>("desktopResolutionPicker");
        picker->setCurrentIndex(picker->findData(resolution));
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        auto *toggle = qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"));
        const auto first = fixture.wordPoint(0), later = fixture.wordPoint(3);
        QVERIFY(first.x() >= 0 && later.x() >= 0);
        const auto position = fixture.state().value("position");
        QVERIFY(fixture.selectWord(page, first));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        auto *headword = qobject_cast<QLabel *>(fixture.widget("rrDictionaryWord"));
        QVERIFY(headword);
        const auto word = headword->text();
        QCOMPARE(word.split(' ', Qt::SkipEmptyParts).size(), 1);
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
        QVERIFY(!page->annotationHandleHitRegion().isEmpty());
        QVERIFY(!toggle->isChecked());
        // Moving a held lookup across other words or past a page edge must
        // retain its original word and consume the whole gesture.
        for (const auto destination : {later, QPoint(10, page->height() / 2),
                                       QPoint(page->width() / 2, page->height() - 10)}) {
            QVERIFY(fixture.click(page, first));
            QTRY_VERIFY(fixture.visible("rrAnnotationCancel"));
            QVERIFY(fixture.press("rrAnnotationCancel"));
            QVERIFY(fixture.drag(page, first, destination, true));
            QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
            QTRY_VERIFY(fixture.visible("rrDictionary"));
            QCOMPARE(headword->text(), word);
            QVERIFY(page->annotationContains(first - QPoint(0, page->topHeight())));
            QVERIFY(!page->annotationContains(later - QPoint(0, page->topHeight())));
            QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
            QVERIFY(!page->annotationHandleHitRegion().isEmpty());
            QCOMPARE(fixture.state().value("position"), position);
            QVERIFY(!toggle->isChecked());
        }
        // Lookup handles adjust a phrase without enabling annotations or turning pages.
        const auto endGrip = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, endGrip, endGrip + later - first));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QTRY_VERIFY(headword->text().split(' ', Qt::SkipEmptyParts).size() >= 3);
        const auto phrase = headword->text();
        const int phraseWords = phrase.split(' ', Qt::SkipEmptyParts).size();
        const auto second = fixture.wordPoint(1);
        const auto startGrip = page->annotationHandleRect(true).center();
        QVERIFY(fixture.drag(page, startGrip, startGrip + second - first));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(fixture.visible("rrDictionary") && headword->text() != phrase);
        QVERIFY(headword->text().split(' ', Qt::SkipEmptyParts).size() < phraseWords);
        QVERIFY(!toggle->isChecked());
        QCOMPARE(fixture.stored("highlights.json", "highlights").size(), 0);
        QCOMPARE(fixture.state().value("position"), position);
        // A lookup grip stays on this page even when dragged past its edge.
        const auto edgeGrip = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, edgeGrip, QPoint(page->width() / 2, page->height() - 2)));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(fixture.press("rrDictionaryClose"));
        QTRY_VERIFY(page->annotationHandleRect(true).isEmpty() && page->annotationHandleRect(false).isEmpty());
        QVERIFY(!page->annotationContains(first - QPoint(0, page->topHeight())));
        QVERIFY(!toggle->isChecked());
        QCOMPARE(fixture.stored("highlights.json", "highlights").size(), 0);
        QVERIFY(fixture.selectWord(page, first));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        // Cancelling a grip must not schedule a lookup that reopens a dismissed sheet.
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        auto *reader = fixture.window.readerWindow();
        const auto grip = page->annotationHandleRect(false).center();
        const auto at = canvas->mapFromScene(page->mapTo(reader, grip));
        const auto moved = at + QPoint(30, 0);
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseMove(canvas->viewport(), moved);
        QTRY_VERIFY(fixture.state().value("annotation_dragging").toBool());
        page->cancelHold();
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, moved);
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        // The geometry query waits behind any earlier lookup work on the engine worker.
        QVERIFY(fixture.wordPoint(1).x() >= 0);
        QVERIFY(!fixture.visible("rrDictionary"));
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(!toggle->isChecked());
        QVERIFY(fixture.click(page, first));
        QTRY_VERIFY(fixture.visible("rrSelectionLookup"));
        QVERIFY(fixture.press("rrSelectionLookup"));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QVERIFY(fixture.click(page, first));
        QVERIFY(!fixture.visible("rrSelectionAnnotate"));
        QVERIFY(!fixture.visible("rrAnnotationStyles"));
        for (const auto *name : {"rrSelectionLookup", "rrAnnotationCancel"}) {
            auto *button = qobject_cast<QAbstractButton *>(fixture.widget(name));
            QVERIFY(button);
            QCOMPARE(ui::elideInterfaceText(button->text(), button->font(), button->width() - 2 * ui::px(16)),
                     button->text());
        }
        QVERIFY(fixture.startAnnotation(page, first));
        QTRY_VERIFY(fixture.visible("rrAnnotationStyle_shaded"));
        QVERIFY(toggle->isChecked());
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        const auto end = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, end, end + later - first));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(page->annotationHandleRect(false).center() != end);
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.click(page, first));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        QVERIFY(fixture.stored("highlights.json", "highlights").first().toObject().value("text").toString().split(' ').size() >= 3);
        QVERIFY(page->annotationHandleRect(true).isEmpty());
        QVERIFY(toggle->isChecked());
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QVERIFY(!toggle->isChecked());
        QVERIFY(!fixture.visible("rrControlsTop"));
        const auto next = fixture.wordPoint(12);
        QVERIFY(next.x() >= 0);
        QVERIFY(fixture.selectWord(page, next));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QCOMPARE(headword->text().split(' ', Qt::SkipEmptyParts).size(), 1);
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
        QVERIFY(!toggle->isChecked());
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(fixture.press("rrDictionaryClose"));
        QTRY_VERIFY(!page->annotationContains(next - QPoint(0, page->topHeight())));
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(!toggle->isChecked());
    }
    void annotationSelection_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("resolution");
        for (int resolution : {300, 0})
            for (bool dark : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(resolution).arg(dark ? "dark" : "light"))) << dark << resolution;
    }
    void annotationSelection() {
        QFETCH(bool, dark);
        QFETCH(int, resolution);
        Fixture fixture(dark);
        auto *resolutionPicker = fixture.window.findChild<QComboBox *>("desktopResolutionPicker");
        QVERIFY(resolutionPicker);
        resolutionPicker->setCurrentIndex(resolutionPicker->findData(resolution));
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        const QPoint first = fixture.wordPoint(0), second = fixture.wordPoint(1), fourth = fixture.wordPoint(3),
                     later = fixture.wordPoint(7);
        QVERIFY(first.x() >= 0 && second.x() >= 0 && fourth.x() >= 0 && later.x() >= 0);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto position = fixture.state().value("position");
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QTRY_VERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(!fixture.visible("rrAnnotationHeader"));
        const int bodyPage = fixture.state().value("page").toInt();
        fixture.window.readerWindow()->pageButton(-1);
        QTRY_VERIFY(fixture.state().value("page").toInt() < bodyPage && !fixture.state().value("busy").toBool());
        const auto division = fixture.wordRect("division"), published = fixture.wordRect("published"),
                   through = fixture.wordRect("through");
        QVERIFY(!division.isEmpty() && !published.isEmpty() && !through.isEmpty());
        QVERIFY(fixture.selectWord(page, division.center()));
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        const QPoint divisionHandle = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, divisionHandle,
                             divisionHandle + QPoint(published.right() - division.right() + 20,
                                                     published.center().y() - division.center().y())));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QVERIFY2(page->annotationContains(published.center() - QPoint(0, page->topHeight())),
                 "Dragging past the line's final word must include published");
        const QPoint divisionStart = page->annotationHandleRect(true).center();
        QVERIFY(fixture.drag(page, divisionStart,
                             divisionStart + QPoint(through.left() - division.left() - 20,
                                                    through.center().y() - division.center().y())));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QVERIFY2(page->annotationContains(through.center() - QPoint(0, page->topHeight())),
                 "Dragging before the line's first word must include through");
        QVERIFY(fixture.click(page, division.center()));
        QTRY_VERIFY(fixture.visible("rrAnnotationCancel"));
        QVERIFY(fixture.press("rrAnnotationCancel"));
        fixture.window.readerWindow()->pageButton(1);
        QTRY_VERIFY(fixture.state().value("page").toInt() == bodyPage && !fixture.state().value("busy").toBool());
        QVERIFY(fixture.selectWord(page, first));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
        const auto originalStart = page->annotationHandleRect(true);
        const auto end = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, end, end + fourth - first));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(page->annotationHandleRect(false).center() != end);
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());
        QCOMPARE(fixture.state().value("position"), position);
        QCOMPARE(page->annotationHandleRect(true), originalStart);
        QVERIFY(page->annotationHandleRect(false).center() != end);

        const auto fixedEnd = page->annotationHandleRect(false);
        const auto start = page->annotationHandleRect(true).center();
        QVERIFY(fixture.drag(page, start, start + second - first));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(page->annotationHandleRect(true) != originalStart);
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QCOMPARE(page->annotationHandleRect(false), fixedEnd);
        QVERIFY(page->annotationHandleRect(true) != originalStart);
        QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());

        const auto extend = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, extend, extend + later - fourth));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(page->annotationHandleRect(false) != fixedEnd);
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.click(page, fourth));
        QTRY_VERIFY(fixture.visible("rrAnnotationStyle_underline"));
        auto *shaded = fixture.widget("rrAnnotationStyle_shaded");
        auto *underline = fixture.widget("rrAnnotationStyle_underline");
        auto *dotted = fixture.widget("rrAnnotationStyle_dotted");
        auto *cancel = fixture.widget("rrAnnotationCancel");
        QVERIFY(!fixture.widget("rrAnnotationDismiss"));
        auto *save = fixture.widget("rrAnnotationSave");
        const auto frame = [page](QWidget *widget) {
            auto *reader = page->window();
            return QRect(widget->mapTo(reader, QPoint()) - page->mapTo(reader, QPoint()), widget->size());
        };
        QCOMPARE(frame(shaded).right() + 1, frame(underline).left());
        QCOMPARE(frame(underline).right() + 1, frame(dotted).left());
        QCOMPARE(frame(save).right() + 1, frame(cancel).left());
        auto *styleSection = fixture.widget("rrAnnotationStyleSection");
        auto *actionsSection = fixture.widget("rrAnnotationActionsSection");
        QVERIFY(styleSection && actionsSection);
        QVERIFY(styleSection->isVisible() && actionsSection->isVisible());
        QVERIFY(frame(styleSection).bottom() < frame(shaded).top());
        if (auto *palette = fixture.widget("rrAnnotationPalette")) {
            auto *colorSection = fixture.widget("rrAnnotationColorSection");
            QVERIFY(colorSection && colorSection->isVisible());
            QCOMPARE(frame(colorSection).bottom() + 1, frame(palette).top());
            QCOMPARE(frame(palette).bottom() + 1, frame(styleSection).top());
            const auto swatches = palette->findChildren<QAbstractButton *>();
            QCOMPARE(swatches.size(), 5);
            int circleTop = -1;
            for (auto *swatch : swatches) {
                const auto image = swatch->grab().toImage();
                const auto paper = swatch->palette().color(QPalette::Window);
                QRect content, color;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        const auto pixel = image.pixelColor(x, y);
                        if (pixel != paper)
                            content |= QRect(x, y, 1, 1);
                        if (qMax(pixel.red(), qMax(pixel.green(), pixel.blue())) -
                            qMin(pixel.red(), qMin(pixel.green(), pixel.blue())) > 30)
                            color |= QRect(x, y, 1, 1);
                    }
                }
                QVERIFY(!content.isEmpty());
                QVERIFY(qAbs(content.top() - (image.height() - 1 - content.bottom())) <= ui::px(10));
                if (!color.isEmpty()) {
                    if (circleTop >= 0)
                        QCOMPARE(color.top(), circleTop);
                    circleTop = color.top();
                }
            }
        }
        QCOMPARE(frame(shaded).bottom() + 1, frame(actionsSection).top());
        QCOMPARE(frame(actionsSection).bottom() + 1, frame(cancel).top());
        QCOMPARE(frame(shaded).left(), frame(fixture.widget("rrAnnotationSave")).left());
        QCOMPARE(frame(dotted).right(), frame(cancel).right());
        for (const auto *name : {"rrAnnotationSave", "rrAnnotationCancel"}) {
            auto *button = dynamic_cast<ui::CaptionButton *>(fixture.widget(name));
            QVERIFY(button && button->isIconAboveLabel());
            QCOMPARE(ui::elideInterfaceText(button->text(), button->font(), button->width() - 2 * ui::px(16)),
                     button->text());
            QVERIFY(ui::metrics::chrome::cellIcon + ui::px(8) + QFontMetrics(button->font()).height() +
                    2 * ui::px(8) <= button->height());
        }
        for (auto *header : {styleSection, actionsSection}) {
            const auto image = header->grab().toImage();
            QCOMPARE(image.pixelColor(image.width() - 10, image.height() / 2), header->palette().color(QPalette::Window));
            QCOMPARE(image.pixelColor(image.width() - 10, image.height() - 1), ui::dividerColor(header));
        }
        const auto paper = shaded->palette().color(QPalette::Window);
        const auto selectedSample = shaded->grab().toImage();
        QCOMPARE(selectedSample.pixelColor(selectedSample.width() / 2, selectedSample.height() - ui::px(6)), paper);

        QVERIFY2(cancel->height() >= QFontMetrics(cancel->font()).height() + 2 * ui::px(20),
                 qPrintable(QString("Action height %1, font height %2, font pixels %3, padding %4")
                     .arg(cancel->height()).arg(QFontMetrics(cancel->font()).height()).arg(cancel->font().pixelSize()).arg(ui::px(20))));

        const QRect selection = page->annotationHandleRect(true).united(page->annotationHandleRect(false));
        auto *card = fixture.widget("rrAnnotationStyles")->parentWidget();
        const auto cardBounds = QRect(page->mapFromGlobal(card->mapToGlobal(QPoint())), card->size());
        QVERIFY(cardBounds.bottom() <= selection.top() || cardBounds.top() >= selection.bottom());
        QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());
        QVERIFY(fixture.press("rrAnnotationStyle_underline"));
        const QRect check(shaded->width() - ui::px(40), ui::px(10), ui::px(24), ui::px(24));
        QVERIFY(selectedSample.copy(check) != shaded->grab().toImage().copy(check));
        const auto selectedUnderline = underline->grab().toImage();
        QCOMPARE(selectedUnderline.pixelColor(selectedUnderline.width() / 2, selectedUnderline.height() - ui::px(6)), paper);
        QVERIFY(fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());
        // A tap on the selection dismisses the open card instead of rebuilding it.
        QVERIFY(fixture.click(page, fourth));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(fixture.click(page, fourth));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotationStyle_underline"))->isChecked());
        for (const auto *name : {"rrAnnotationPalette", "rrAnnotationStyles", "rrAnnotationSave", "rrAnnotationCancel"}) {
            auto *item = fixture.widget(name);
            if (!item)
                continue; // Grayscale omits the color palette.
            QVERIFY(item->isVisible());
            QVERIFY(item->visibleRegion().contains(item->rect()));
            QVERIFY(card->rect().contains(QRect(item->mapTo(card, QPoint()), item->size())));
        }
        QVERIFY(fixture.click(page, QPoint(10, page->height() / 2)));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(fixture.click(page, fourth));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        const auto saved = fixture.stored("highlights.json", "highlights").first().toObject();
        QCOMPARE(saved.value("style").toString(), QString("underline"));
        QVERIFY(saved.value("text").toString().split(' ').size() >= 4);
        QVERIFY(page->annotationHandleRect(true).isEmpty() && page->annotationHandleRect(false).isEmpty());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrControlsTop"));
        // Outside annotation mode, saved passages use ordinary word lookup.
        QVERIFY(fixture.press("rrAnnotate"));
        QVERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(fixture.selectWord(page, second));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QVERIFY(!fixture.visible("rrAnnotationEdit"));
        QVERIFY(!fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrDictionaryClose"));
        QVERIFY(page->annotationHandleRect(true).isEmpty() && page->annotationHandleRect(false).isEmpty());
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));

        const auto previousPage = fixture.state().value("page").toInt();
        QVERIFY(fixture.drag(page, first, first - QPoint(qMax(120, page->width() / 4), 0)));
        QTRY_VERIFY(fixture.state().value("page").toInt() != previousPage);
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject(), saved);
        fixture.window.readerWindow()->pageButton(-1);
        QTRY_VERIFY(fixture.state().value("page").toInt() == previousPage && !fixture.state().value("busy").toBool());
        QVERIFY(fixture.drag(page, QPoint(page->width() - 10, page->height() / 2),
                             QPoint(page->width() / 2, page->height() / 2)));
        QTRY_VERIFY(fixture.state().value("page").toInt() != previousPage);
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
    }
    void coordinateProbeSerialization() {
        Fixture fixture;
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        struct Gate {
            QSemaphore release;
            std::atomic_bool entered{false}, finished{false};
        };
        auto gate = std::make_shared<Gate>();
        auto unblock = qScopeGuard([gate] { gate->release.release(); });
        qt::invoke(engineWorker(), [gate] {
            EngineJob activity;
            gate->entered.store(true);
            gate->release.acquire();
            gate->finished.store(true);
        }, Qt::QueuedConnection);
        QTRY_VERIFY(gate->entered.load());
        qt::later(20, qApp, [gate] { gate->release.release(); });
        const auto result = fixture.coordinateQuery(book, QPair<bool, bool>{false, false}, [gate](ReaderModel &model) {
            return qMakePair(QThread::currentThread() == engineWorker()->thread(),
                             gate->finished.load() && model.pages() > 0);
        });
        QVERIFY2(result.first, "Coordinate probes must use the reader's engine worker");
        QVERIFY2(result.second, "A probe must wait behind existing engine work without blocking GUI callbacks");
    }
    void annotationGripMenus_data() {
        QTest::addColumn<bool>("toolbarEntry");
        QTest::newRow("lookup-then-toolbar-native") << false;
        QTest::newRow("annotation-300") << true;
    }
    void annotationGripMenus() {
        QFETCH(bool, toolbarEntry);
        Fixture fixture(toolbarEntry);
        auto *picker = fixture.window.findChild<QComboBox *>("desktopResolutionPicker");
        picker->setCurrentIndex(picker->findData(toolbarEntry ? 300 : 0));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto first = fixture.wordPoint(), second = fixture.wordPoint(1), fourth = fixture.wordPoint(3);
        QVERIFY(first.x() >= 0 && second.x() >= 0 && fourth.x() >= 0);
        const auto position = fixture.state().value("position");
        if (toolbarEntry) {
            QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrAnnotate"));
        }
        QVERIFY(fixture.selectWord(page, first));
        if (!toolbarEntry) {
            QTRY_VERIFY(fixture.visible("rrDictionary"));
            QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
            QVERIFY(!fixture.visible("rrSelectionAnnotate"));
            QVERIFY(fixture.startAnnotation(page, first));
            QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        }
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        for (const bool start : {false, true}) {
            // Grips stay usable through the marking palette for either entry route.
            if (!fixture.visible("rrAnnotationPopover"))
                QVERIFY(fixture.click(page, first));
            QTRY_VERIFY(fixture.visible("rrAnnotationPopover"));
            const auto moving = page->annotationHandleRect(start);
            const auto fixed = page->annotationHandleRect(!start);
            QVERIFY(!moving.isEmpty() && !fixed.isEmpty());
            bool recognized = false;
            auto original = page->annotationDragStarted;
            page->annotationDragStarted = [&](QPoint point, int boundary) {
                recognized = boundary == (start ? 0 : 1);
                return original(point, boundary);
            };
            const bool dragged = fixture.drag(page, moving.center(),
                moving.center() + (start ? second : fourth) - first);
            page->annotationDragStarted = original;
            QVERIFY(dragged);
            QVERIFY2(recognized, "An open selection menu must pass handle drags to the page");
            QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
            QTRY_VERIFY(page->annotationHandleRect(start) != moving);
            QCOMPARE(page->annotationHandleRect(!start), fixed);
            QCOMPARE(fixture.state().value("position"), position);
            QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());
            QVERIFY(!fixture.visible("rrAnnotationPopover"));
        }
        if (!fixture.visible("rrAnnotationPopover"))
            QVERIFY(fixture.click(page, second));
        QTRY_VERIFY(fixture.visible("rrAnnotationPopover"));
        const auto kept = page->annotationHandleRect(true);
        QVERIFY(fixture.click(page, QPoint(10, page->height() / 2)));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QCOMPARE(page->annotationHandleRect(true), kept);
        QCOMPARE(fixture.state().value("position"), position);
        QCOMPARE(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked(), true);
    }
    void annotationCrossPageDrag_data() {
        QTest::addColumn<bool>("startHandle");
        QTest::addColumn<int>("direction");
        QTest::newRow("start-backward") << true << -1;
        QTest::newRow("start-forward") << true << 1;
        QTest::newRow("end-backward") << false << -1;
        QTest::newRow("end-forward") << false << 1;
    }
    void annotationCrossPageDrag() { checkCrossPageDrag(true); }
    void selectionCrossPageDrag_data() { annotationCrossPageDrag_data(); }
    void selectionCrossPageDrag() { checkCrossPageDrag(false); }
    void annotationSectionBoundary_data() { annotationCrossPageDrag_data(); }
    void annotationSectionBoundary() { checkCrossPageDrag(true, true); }
    void checkCrossPageDrag(bool toolbarEntry, bool sectionBoundary = false) {
        QFETCH(bool, startHandle);
        QFETCH(int, direction);
        Fixture fixture(startHandle);
        auto *picker = fixture.window.findChild<QComboBox *>("desktopResolutionPicker");
        picker->setCurrentIndex(picker->findData(startHandle ? 0 : 300));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *reader = fixture.window.readerWindow();
        // Use chapter-interior pages for continuation, and the two sides of
        // the Prologue/Chapter One boundary for the section clamp.
        const int advance = sectionBoundary ? (direction > 0 ? 1 : 2) : 3;
        for (int i = 0; i < advance; ++i) {
            reader->pageButton(1);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
        }
        const int origin = fixture.state().value("page").toInt();
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        if (toolbarEntry) {
            QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrAnnotate"));
        }
        const auto first = fixture.wordPoint();
        QVERIFY(first.x() >= 0);
        QVERIFY(fixture.selectWord(page, first));
        if (!toolbarEntry) {
            QTRY_VERIFY(fixture.visible("rrDictionary"));
            QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
            QVERIFY(!fixture.visible("rrSelectionAnnotate"));
            QVERIFY(fixture.startAnnotation(page, first));
            QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        }
        QTRY_VERIFY(!page->annotationHandleRect(startHandle).isEmpty());
        const auto edges = fixture.pageEdges();
        QCOMPARE(direction > 0 ? edges.continuesAfter : edges.continuesBefore, !sectionBoundary);
        QVERIFY(!edges.first.text.isEmpty() && !edges.last.text.isEmpty());
        const auto edge = direction > 0 ? edges.end : edges.start;
        QVERIFY(edge.visible);
        const QPoint target = (edge.first + edge.last) / 2 + edge.outward * ui::px(20);
        const auto handle = page->annotationHandleRect(startHandle).center();
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        const auto map = [&](QPoint point) { return canvas->mapFromScene(page->mapTo(reader, point)); };
        // Preserve the handle-to-word offset used by the production pointer path.
        const QPoint offset = handle - fixture.selectionHandleTarget(first, startHandle);
        const QPoint exit = target + QPoint(0, page->topHeight()) + offset;
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, map(handle));
        QTest::mouseMove(canvas->viewport(), map(exit));
        if (sectionBoundary) {
            QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
            QTest::qWait(800);
            QCOMPARE(fixture.state().value("page").toInt(), origin);
            const auto boundary = direction > 0 ? edges.last : edges.first;
            QVERIFY(page->annotationContains(boundary.rectangles.first().center()));
            QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, map(exit));
            QCOMPARE(fixture.state().value("page").toInt(), origin);
            return;
        }
        const bool turned = QTest::qWaitFor([&] { return fixture.state().value("page").toInt() == origin + direction &&
                                                         !fixture.state().value("busy").toBool(); }, 5000);
        if (!turned)
            QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, map(exit));
        QVERIFY(turned);
        QTRY_VERIFY(!(direction > 0 ? page->annotationHandleRect(false) : page->annotationHandleRect(true)).isEmpty());
        auto destination = fixture.pageEdges();
        auto selected = direction > 0 ? destination.first : destination.last;
        if (direction < 0)
            QVERIFY2(!selected.punctuation, "Backward extension must stop on the last word, not punctuation");
        QVERIFY(page->annotationContains(selected.rectangles.first().center()));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        const auto stoppedStart = page->annotationHandleRect(true), stoppedEnd = page->annotationHandleRect(false);
        // Keep the same press held past the long-press delay, then move back
        // into the text and out again. None of this may restart the stopped drag.
        QTest::qWait(800);
        const auto inside = fixture.wordPoint(2);
        QTest::mouseMove(canvas->viewport(), map(inside + offset));
        const auto destinationEdge = direction > 0 ? destination.end : destination.start;
        const auto beyond = (destinationEdge.first + destinationEdge.last) / 2 +
                            destinationEdge.outward * ui::px(20) + QPoint(0, page->topHeight()) + offset;
        QTest::mouseMove(canvas->viewport(), map(beyond));
        QTest::qWait(200);
        QCOMPARE(fixture.state().value("page").toInt(), origin + direction);
        QVERIFY(!fixture.state().value("annotation_dragging").toBool());
        QCOMPARE(page->annotationHandleRect(true), stoppedStart);
        QCOMPARE(page->annotationHandleRect(false), stoppedEnd);
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, map(beyond));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        int expectedPage = origin + direction;
        QCOMPARE(fixture.state().value("page").toInt(), expectedPage);
        if (!startHandle && direction > 0) {
            // A fresh grip drag can extend the same selection to another page.
            const auto grip = page->annotationHandleRect(false).center();
            const auto selectedPoint = selected.rectangles.first().center() + QPoint(0, page->topHeight());
            const auto freshOffset = grip - fixture.selectionHandleTarget(selectedPoint, false);
            const auto nextExit = (destination.end.first + destination.end.last) / 2 +
                                  destination.end.outward * ui::px(20) + QPoint(0, page->topHeight()) + freshOffset;
            QVERIFY(fixture.drag(page, grip, nextExit));
            ++expectedPage;
            QTRY_VERIFY(fixture.state().value("page").toInt() == expectedPage && !fixture.state().value("busy").toBool());
            QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
            destination = fixture.pageEdges();
            selected = destination.first;
            QTRY_VERIFY(page->annotationContains(selected.rectangles.first().center()));
        }
        // Both explicit entry routes keep annotation mode active after saving.
        if (!fixture.visible("rrAnnotationPopover"))
            QVERIFY(fixture.click(page, selected.rectangles.first().center() + QPoint(0, page->topHeight())));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        QVERIFY(fixture.stored("highlights.json", "highlights").first().toObject().value("text").toString().size() > 100);
        QVERIFY(fixture.press("rrPage"));
        auto *toggle = qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"));
        QVERIFY(toggle);
        QCOMPARE(toggle->isChecked(), true);
    }
    void annotationCrossPageOverlap_data() {
        QTest::addColumn<int>("direction");
        QTest::newRow("backward") << -1;
        QTest::newRow("forward") << 1;
    }
    void annotationCrossPageOverlap() {
        QFETCH(int, direction);
        Fixture fixture;
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *reader = fixture.window.readerWindow();
        for (int i = 0; i < 3; ++i) {
            reader->pageButton(1);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
        }
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto edges = fixture.pageEdges();
        const auto word = direction > 0 ? edges.first : edges.last;
        QVERIFY(!word.rectangles.isEmpty());
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        const auto marked = word.rectangles.first().center() + QPoint(0, page->topHeight());
        QVERIFY(fixture.selectWord(page, marked));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
        QVERIFY(fixture.click(page, marked));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        const auto saved = fixture.stored("highlights.json", "highlights");
        reader->pageButton(-direction);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const int origin = fixture.state().value("page").toInt();
        const auto first = fixture.wordPoint();
        QVERIFY(fixture.selectWord(page, first));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
        const bool start = direction < 0;
        const auto handle = page->annotationHandleRect(start).center();
        const auto bounds = fixture.pageEdges();
        const auto edge = direction > 0 ? bounds.end : bounds.start;
        const auto exit = (edge.first + edge.last) / 2 + edge.outward * ui::px(20) +
                          QPoint(0, page->topHeight()) + handle - fixture.selectionHandleTarget(first, start);
        QVERIFY(fixture.drag(page, handle, exit));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool() && !fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("page").toInt(), origin);
        QTRY_VERIFY(!page->annotationHandleRect(start).isEmpty());
        QVERIFY(page->annotationContains(first - QPoint(0, page->topHeight())));
        QVERIFY(!fixture.visible("rrAnnotationOverlap"));
        QCOMPARE(fixture.stored("highlights.json", "highlights"), saved);
        // Moves before the boundary remain valid. The blocked turn must not
        // include the saved passage, and the remaining draft can still be saved.
        QVERIFY(fixture.click(page, first));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 2);
        QVERIFY(fixture.stored("highlights.json", "highlights").contains(saved.first()));
    }
    void annotationOverlap_data() { bookmarks_data(); }
    void annotationOverlap() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const QPoint first = fixture.wordPoint(0), second = fixture.wordPoint(1), fourth = fixture.wordPoint(3),
                     beyond = fixture.wordPoint(12);
        QVERIFY(first.x() >= 0 && second.x() >= 0 && fourth.x() >= 0 && beyond.x() >= 0);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.selectWord(page, second));
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        const QPoint handle = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, handle, handle + fourth - second));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QVERIFY(fixture.click(page, second));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        const auto saved = fixture.stored("highlights.json", "highlights").first().toObject();
        // Both annotation entry routes reject crossing another saved passage.
        // Check both handles, including moves beyond the whole mark.
        for (bool marking : {true, false}) {
            for (bool fromLeft : {true, false}) {
                const QPoint word = fromLeft ? first : beyond;
                QVERIFY(fixture.selectWord(page, word));
                if (!marking) {
                    QTRY_VERIFY(fixture.visible("rrDictionary"));
                    QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
                    QVERIFY(!fixture.visible("rrSelectionAnnotate"));
                    QVERIFY(fixture.startAnnotation(page, word));
                }
                QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
                const auto start = page->annotationHandleRect(true), end = page->annotationHandleRect(false);
                const QPoint grip = (fromLeft ? end : start).center();
                QVERIFY(fixture.drag(page, grip, grip + (fromLeft ? beyond : first) - word));
                QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
                QCOMPARE(fromLeft ? page->annotationHandleRect(true) : page->annotationHandleRect(false),
                         fromLeft ? start : end);
                QVERIFY(!page->annotationContains(second - QPoint(0, page->topHeight())));
                QVERIFY(!page->annotationContains(fourth - QPoint(0, page->topHeight())));
                QVERIFY(!fixture.visible("rrAnnotationOverlap"));
                QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject(), saved);
                if (!fixture.visible("rrAnnotationCancel"))
                    QVERIFY(fixture.click(page, word));
                QTRY_VERIFY(fixture.visible("rrAnnotationCancel"));
                QVERIFY(fixture.press("rrAnnotationCancel"));
                if (!marking) {
                    QVERIFY(fixture.press("rrPage"));
                    QVERIFY(fixture.press("rrAnnotate"));
                    QVERIFY(!fixture.visible("rrControlsTop"));
                }
            }
            if (marking) {
                QVERIFY(fixture.press("rrPage"));
                QVERIFY(fixture.press("rrAnnotate"));
                QVERIFY(!fixture.visible("rrControlsTop"));
            }
        }
        // Adjust may change its own saved range, but cannot cross another mark.
        QVERIFY(fixture.selectWord(page, beyond));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
        QVERIFY(!fixture.visible("rrSelectionAnnotate"));
        QVERIFY(fixture.startAnnotation(page, beyond));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 2);
        const auto highlights = fixture.stored("highlights.json", "highlights");
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QVERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(!qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QVERIFY(fixture.selectWord(page, beyond));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QVERIFY(!fixture.visible("rrAnnotationEdit"));
        QVERIFY(fixture.startAnnotation(page, beyond));
        QTRY_VERIFY(fixture.visible("rrAnnotationEdit"));
        QVERIFY(!fixture.visible("rrAnnotationSave"));
        QVERIFY(page->annotationHandleRect(true).isEmpty() && page->annotationHandleRect(false).isEmpty());
        QVERIFY(fixture.press("rrAnnotationEdit"));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
        const QPoint grip = page->annotationHandleRect(true).center();
        QVERIFY(fixture.drag(page, grip, grip + first - beyond));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QVERIFY(!page->annotationContains(second - QPoint(0, page->topHeight())));
        QVERIFY(!page->annotationContains(fourth - QPoint(0, page->topHeight())));
        QVERIFY(!fixture.visible("rrAnnotationOverlap"));
        QCOMPARE(fixture.stored("highlights.json", "highlights"), highlights);
    }
    void headerFooterDismissal_data() { bookmarks_data(); }
    void headerFooterDismissal() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(0);
        fixture.window.resize(1000, 1600);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavLayout"));
        QVERIFY(fixture.press("rrHeaderFooter"));
        QTRY_VERIFY(fixture.visible("rrHeaderFooterView"));
        const auto position = fixture.state().value("position");

        auto *screenshot = fixture.window.findChild<QPushButton *>("desktopScreenshot");
        QVERIFY(screenshot && screenshot->isVisible());
        {
            // Exercise the real toolbar click without writing to the user's
            // Desktop. Use the same capture method with a temporary directory.
            const QSignalBlocker blocked(screenshot);
            QTest::mouseClick(screenshot, Qt::LeftButton);
        }
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        QTemporaryDir captures;
        QVERIFY(captures.isValid());
        QString error;
        const auto captured = fixture.window.saveScreenshot(captures.path(), &error);
        QVERIFY2(!captured.isEmpty(), qPrintable(error));
        QVERIFY(!QImage(captured).isNull());
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        auto *frame = fixture.window.findChild<QWidget *>("desktopPreviewToolbar");
        QTest::mouseClick(frame, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        QTest::mouseClick(&fixture.window, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
        QVERIFY(fixture.visible("rrHeaderFooterView"));

        auto *page = fixture.widget("rrPage");
        auto *reader = fixture.window.readerWindow();
        QPoint exposed(-1, -1);
        for (int y = 0; y < page->height(); y += 10) {
            const QPoint point(page->width() / 2, y);
            if (reader->childAt(page->mapTo(reader, point)) == page) {
                exposed = point;
                break;
            }
        }
        QVERIFY(exposed.x() >= 0);
        QVERIFY(fixture.click(page, exposed));
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        auto *device = QTest::createTouchDevice();
        auto touch = QTest::touchEvent(page, device, false);
        touch.press(0, exposed).commit();
        touch.release(0, exposed).commit();
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        QVERIFY(fixture.press("rrNavText"));
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(fixture.press("rrHeaderFooterPreset_alternative_a"));
        QTRY_COMPARE(fixture.state().value("header_footer_preset").toString(), QString("alternative_a"));
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        QVERIFY(fixture.press("rrHeaderFooterClose"));
        QTRY_VERIFY(!fixture.visible("rrHeaderFooterView"));
        QVERIFY(fixture.visible("rrHeaderFooter"));
    }
    void annotationModeFeedback_data() { bookmarks_data(); }
    void annotationModeFeedback() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        for (const bool enabled : {true, false, true, false}) {
            QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(fixture.visible("rrAnnotate"));
            QVERIFY(fixture.press("rrAnnotate"));
            QTRY_VERIFY(!fixture.visible("rrControlsTop"));
            QCOMPARE(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked(), enabled);
            QTRY_VERIFY(fixture.visible("rrAnnotationModeNotice"));
            auto *notice = fixture.widget("rrAnnotationModeNotice");
            QVERIFY(notice->testAttribute(Qt::WA_TransparentForMouseEvents));
            QCOMPARE(notice->findChild<QLabel *>("rrAnnotationModeState")->text(),
                     strings::get(enabled ? "common.on" : "common.off"));
            QTRY_VERIFY(!fixture.visible("rrAnnotationModeNotice"));
        }
        auto *page = fixture.widget("rrPage");
        QVERIFY(fixture.selectWord(page, fixture.wordPoint()));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QVERIFY(!fixture.visible("rrAnnotationModeNotice"));
    }
    void annotations() {
        QFETCH(bool, dark);
        QFETCH(bool, pickerExport);
        QFETCH(bool, markdownExport);
        const char *exportAction = markdownExport ? "rrAnnotationExportMarkdown" : "rrAnnotationExportHtml";
        Fixture fixture(dark, false, false, true, !pickerExport);
        QVERIFY(fixture.prepared);
        QVERIFY(storage::writeJson(fixture.paths.preferences(), {{"dark_mode", dark}, {"highlight_help_seen", false}}));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        const QPoint word = fixture.wordPoint();
        QVERIFY(word.x() >= 0);
        const QPoint end = fixture.wordPoint(1);
        QVERIFY(end.x() >= 0 && end != word);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto position = fixture.state().value("position");
        const auto image = page->image;
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrAnnotate"));
        QVERIFY(fixture.press("rrAnnotate"));
        QTRY_VERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(!fixture.window.readerWindow()->findChild<QWidget *>("rrAnnotationHeader"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QCOMPARE(page->image, image);
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(page->annotationHandleRect(true).isEmpty());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrControlsTop"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(!fixture.visible("rrAnnotationHeader"));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        const QRect draftStart = page->annotationHandleRect(true), draftEnd = page->annotationHandleRect(false);
        QVERIFY(fixture.click(page, QPoint(page->width() / 2, page->height() / 2)));
        QTRY_VERIFY(fixture.visible("rrControlsTop"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_COMPARE(page->annotationHandleRect(true), draftStart);
        QTRY_COMPARE(page->annotationHandleRect(false), draftEnd);
        QVERIFY(fixture.click(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationColor_yellow"));
        QVERIFY(fixture.press("rrAnnotationColor_yellow"));
        QVERIFY(fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        const auto saved = fixture.stored("highlights.json", "highlights").first().toObject();
        QVERIFY(!saved.value("text").toString().isEmpty());
        QVERIFY(!saved.value("start").toString().isEmpty());
        QVERIFY(!saved.value("end").toString().isEmpty());
        QCOMPARE(fixture.state().value("position"), position);
        QCOMPARE(page->image, image);
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationEdit"));
        QTRY_VERIFY(fixture.visible("rrAnnotationColor_blue"));
        QVERIFY(fixture.press("rrAnnotationColor_blue"));
        QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject(), saved);
        {
            BlockedFile blocked(fixture.storedPath("highlights.json"));
            QVERIFY(blocked.active());
            QVERIFY(fixture.press("rrAnnotationApply"));
            QTRY_VERIFY(fixture.visible("rrAnnotationStyleError"));
            QVERIFY(!fixture.window.readerWindow()->findChild<QLabel *>("rrAnnotationStyleError")->text().isEmpty());
            QVERIFY(blocked.restore());
        }
        QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject(), saved);
        QVERIFY(fixture.press("rrAnnotationApply"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").first().toObject().value("color").toString(), QString("blue"));
        QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject().value("id"), saved.value("id"));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationEdit"));
        QVERIFY(!fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationEdit"));
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        const auto handle = page->annotationHandleRect(false).center();
        // Expansion requires crossing the next word's far edge. A distance
        // between word centres can stop inside a longer word after reflow.
        const QPoint target = handle + fixture.selectionHandleTarget(end, false) -
                              fixture.selectionHandleTarget(word, false) + QPoint(ui::px(8), 0);
        QVERIFY(fixture.drag(page, handle, target));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        QTRY_VERIFY(page->annotationContains(end - QPoint(0, page->topHeight())));
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.click(page, end));
        QTRY_VERIFY(fixture.visible("rrAnnotationStyle_dotted"));
        QVERIFY(fixture.press("rrAnnotationStyle_dotted"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QCOMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        QTRY_VERIFY(fixture.stored("highlights.json", "highlights").first().toObject().value("text") != saved.value("text"));
        QVERIFY(fixture.stored("highlights.json", "highlights").first().toObject().value("text").toString().contains(saved.value("text").toString()));
        QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject().value("id"), saved.value("id"));
        QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject().value("style").toString(), QString("dotted"));
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationColor_blue"));
        QVERIFY(fixture.press("rrAnnotationColor_blue"));
        QVERIFY(fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.press("rrAnnotationApply"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").first().toObject().value("style").toString(), QString("dotted"));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrControlsTop"));
        QVERIFY(fixture.press("rrAnnotate"));
        QTRY_VERIFY(!fixture.visible("rrAnnotationHeader"));
        QVERIFY(!qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QVERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QTRY_VERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrControlsTop"));
        QVERIFY(fixture.press("rrAnnotate"));
        QTRY_VERIFY(!fixture.visible("rrAnnotationHeader"));
        QVERIFY(!qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QTRY_VERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrBookmarks"));
        QVERIFY(fixture.press("rrBookmarks"));
        auto *annotationsTab = qobject_cast<QPushButton *>(fixture.widget("rrAnnotationsTab"));
        auto *bookmarksTab = qobject_cast<QPushButton *>(fixture.widget("rrBookmarksTab"));
        QVERIFY(annotationsTab && bookmarksTab);
        QVERIFY(annotationsTab->isChecked());
        QTRY_VERIFY(fixture.widget("rrAnnotationMore")->isEnabled());
        QVERIFY(!fixture.widget("rrAnnotationExportHtml"));
        QVERIFY(fixture.widget("rrAnnotationMore")->x() > fixture.widget("rrMarksSort")->x());
        QVERIFY(fixture.press("rrAnnotationMore"));
        QTRY_VERIFY(fixture.visible("rrAnnotationExportHtml"));
        QCOMPARE(qobject_cast<QPushButton *>(fixture.widget("rrAnnotationExportHtml"))->text(), strings::get("annotations.export_html"));
        auto *actions = fixture.widget("rrAnnotationActions");
        auto *reader = fixture.window.readerWindow();
        QVERIFY(actions && actions->parentWidget() == reader);
        QCOMPARE(actions->geometry(), reader->rect());
        QVERIFY(!fixture.widget("rrBookmarkExport"));
        QVERIFY(fixture.widget("rrAnnotationExportMarkdown")->isEnabled());
        if (!pickerExport) {
            auto *canvas = fixture.window.findChild<QGraphicsView *>();
            QVERIFY(canvas);
            for (const char *name : {"rrAnnotationExportMarkdown", "rrAnnotationExportHtml"}) {
                QPointer<QPushButton> action = qobject_cast<QPushButton *>(fixture.widget(name));
                QVERIFY(action);
                const auto sample = [&] {
                    const auto image = canvas->viewport()->grab().toImage();
                    const auto point = canvas->mapFromScene(action->mapTo(reader, QPoint(action->width() - 10, action->height() / 3)));
                    return image.pixelColor(qRound(point.x() * image.devicePixelRatio()), qRound(point.y() * image.devicePixelRatio()));
                };
                const auto normal = sample();
                const auto point = canvas->mapFromScene(action->mapTo(reader, action->rect().center()));
                const QSignalBlocker holdAction(action.data());
                QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
                QTRY_VERIFY(action->isDown());
                QTest::qWait(50);
                const auto pressed = sample();
                QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
                QTRY_VERIFY(action && !action->isDown());
                QTRY_COMPARE(sample(), normal);
                QCOMPARE(pressed, QColor(dark ? Qt::white : Qt::black));
            }
        }
        const int currentPage = fixture.state().value("page").toInt();
        reader->pageButton(1);
        QCOMPARE(fixture.state().value("page").toInt(), currentPage);
        QVERIFY(fixture.click(actions, QPoint(5, 5)));
        QTRY_VERIFY(!fixture.widget("rrAnnotationActions"));
        QVERIFY(!fixture.visible("rrAnnotationExportConfirm"));
        QVERIFY(fixture.press("rrAnnotationMore"));
        QTRY_VERIFY(fixture.visible("rrAnnotationExportHtml"));
        actions = fixture.widget("rrAnnotationActions");
        auto *bookmarksTabWidget = fixture.widget("rrBookmarksTab");
        QVERIFY(fixture.click(actions, actions->mapFrom(reader, bookmarksTabWidget->mapTo(reader, bookmarksTabWidget->rect().center()))));
        QTRY_VERIFY(!fixture.widget("rrAnnotationActions"));
        QVERIFY(annotationsTab->isChecked());
        QVERIFY(fixture.pressAnnotationExport(exportAction));
        QTRY_VERIFY(fixture.visible("rrAnnotationExportConfirm"));
        QVERIFY(!fixture.widget("rrAnnotationExportPicker"));
        QVERIFY(fixture.press("rrAnnotationExportCancel"));
        QTRY_VERIFY(!fixture.widget("rrAnnotationExportDialog"));
        if (pickerExport) {
            QVERIFY(fixture.pressAnnotationExport(exportAction));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportConfirm"));
            QVERIFY(fixture.press("rrAnnotationExportConfirm"));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportPicker"));
            auto *picker = qobject_cast<QFileDialog *>(fixture.widget("rrAnnotationExportPicker"));
            QVERIFY(picker && picker->selectedFiles().value(0).endsWith(".zip"));
            QVERIFY(!picker->testOption(QFileDialog::DontConfirmOverwrite));
            QCOMPARE(picker->parentWidget(), static_cast<QWidget *>(&fixture.window));
            QVERIFY(!picker->graphicsProxyWidget());
            QPointer<QFileDialog> cancelledPicker = picker;
            picker->reject();
            QTRY_VERIFY(cancelledPicker.isNull());
            QVERIFY(!fixture.visible("rrAnnotationExportDone"));
        }
        QVERIFY(!QDir(fixture.paths.directory + "/exports/annotations").exists());
        QVERIFY(fixture.pressAnnotationExport(exportAction));
        QTRY_VERIFY(fixture.visible("rrAnnotationExportConfirm"));
        QVERIFY(fixture.press("rrAnnotationExportConfirm"));
        QString exportedPath;
        QPointer<QFileDialog> exportPicker;
        if (pickerExport) {
            QTRY_VERIFY(fixture.visible("rrAnnotationExportPicker"));
            auto *picker = qobject_cast<QFileDialog *>(fixture.widget("rrAnnotationExportPicker"));
            QVERIFY(picker);
            exportPicker = picker;
            QCOMPARE(picker->parentWidget(), static_cast<QWidget *>(&fixture.window));
            QVERIFY(!picker->graphicsProxyWidget());
            exportedPath = fixture.directory.path() + "/chosen annotations.zip";
            auto *filename = picker->findChild<QLineEdit *>("fileNameEdit");
            QVERIFY(filename);
            filename->setFocus();
            filename->setText(exportedPath);
            QTRY_COMPARE(picker->selectedFiles().value(0), exportedPath);
            QVERIFY(QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection));
        } else {
            const QDir exportFolder(fixture.paths.directory + "/exports/annotations");
            const auto exportedFiles = exportFolder.entryList({"*.zip"}, QDir::Files);
            QCOMPARE(exportedFiles.size(), 1);
            exportedPath = exportFolder.filePath(exportedFiles.first());
        }
        QTRY_VERIFY(fixture.visible("rrAnnotationExportDone"));
        QTRY_VERIFY(!fixture.widget("rrAnnotationExportDialog"));
        QTRY_VERIFY(exportPicker.isNull());
        QFile exportedFile(exportedPath);
        QVERIFY(exportedFile.open(QIODevice::ReadOnly));
        const auto originalZip = exportedFile.readAll();
        QVERIFY(originalZip.startsWith("PK"));
        exportedFile.close();
        ZipResource exportedHtml;
        QVERIFY(exportedHtml.open(exportedPath, markdownExport ? "annotations.md" : "annotations.html"));
        QByteArray exportedMarkup;
        while (!exportedHtml.finished() && exportedHtml.valid()) exportedMarkup += exportedHtml.next();
        QVERIFY(exportedHtml.valid() && exportedHtml.finished());
        QCOMPARE(originalZip.mid(originalZip.size() - 14, 4), QByteArray::fromHex("01000100"));
        if (markdownExport) {
            QVERIFY(exportedMarkup.startsWith("# ") && exportedMarkup.contains("### Annotation 001"));
        } else {
            QVERIFY(exportedMarkup.contains("data:font/ttf;base64,"));
            QVERIFY(exportedMarkup.contains(">#1</a>"));
        }
        QVERIFY(fixture.press("rrAnnotationExportDone"));
        if (!pickerExport) {
            QVERIFY(fixture.pressAnnotationExport(exportAction));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportReplace"));
            QVERIFY(!fixture.visible("rrAnnotationExportConfirm"));
            QVERIFY(fixture.press("rrAnnotationExportCancel"));
            QTRY_VERIFY(!fixture.widget("rrAnnotationExportReplaceDialog"));
            QFile preserved(exportedPath);
            QVERIFY(preserved.open(QIODevice::ReadOnly));
            QCOMPARE(preserved.readAll(), originalZip);
            preserved.close();
            QFile replaceTarget(exportedPath);
            QVERIFY(replaceTarget.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(replaceTarget.write("replace this existing export"), qint64(28));
            replaceTarget.close();
            QVERIFY(fixture.pressAnnotationExport(exportAction));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportReplace"));
            QVERIFY(fixture.press("rrAnnotationExportReplace"));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportDone"));
            QTRY_VERIFY(!fixture.widget("rrAnnotationExportReplaceDialog"));
            QFile replaced(exportedPath);
            QVERIFY(replaced.open(QIODevice::ReadOnly));
            const auto replacementZip = replaced.readAll();
            QVERIFY(replacementZip.startsWith("PK"));
            QVERIFY(replacementZip != QByteArray("replace this existing export"));
            replaced.close();
            QVERIFY(fixture.press("rrAnnotationExportDone"));
            // A file created after the new-export prompt must also need approval.
            QVERIFY(QFile::remove(exportedPath));
            QVERIFY(fixture.pressAnnotationExport(exportAction));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportConfirm"));
            QFile appeared(exportedPath);
            QVERIFY(appeared.open(QIODevice::WriteOnly));
            QCOMPARE(appeared.write("keep this existing file"), qint64(23));
            appeared.close();
            QVERIFY(fixture.press("rrAnnotationExportConfirm"));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportReplace"));
            QTRY_VERIFY(!fixture.widget("rrAnnotationExportDialog"));
            QVERIFY(fixture.press("rrAnnotationExportCancel"));
            QTRY_VERIFY(!fixture.widget("rrAnnotationExportReplaceDialog"));
            QVERIFY(appeared.open(QIODevice::ReadOnly));
            QCOMPARE(appeared.readAll(), QByteArray("keep this existing file"));
        }
        QVERIFY(fixture.press("rrMarksSort"));
        for (const auto *name : {"rrBookmarkList", "rrMarksSortPopup", "rrModalCard", "rrMarksSort_newest"}) {
            auto *probe = fixture.widget(name);
            QVERIFY(probe);
            if (QString::fromLatin1(name) == "rrMarksSortPopup")
                continue;
            QCOMPARE(probe->palette().color(QPalette::Window), QColor(dark ? Qt::black : Qt::white));
            QCOMPARE(probe->palette().color(QPalette::WindowText), QColor(dark ? Qt::white : Qt::black));
        }
        QVERIFY(fixture.press("rrMarksSort_newest"));
        QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value("marks_sort").toString(), QString("newest"));
        QTRY_VERIFY(!fixture.widget("rrMarksSortPopup"));
        QVERIFY(fixture.press("rrBookmarksTab"));
        QTRY_VERIFY(fixture.visible("rrBookmark0"));
        QVERIFY(fixture.press("rrBookmarkListClose"));
        QVERIFY(fixture.press("rrBookmarks"));
        QVERIFY(annotationsTab->isChecked());
        QVERIFY(fixture.press("rrMarksSort"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrMarksSort_newest"))->isChecked());
        QVERIFY(fixture.press("rrMarksSort_newest"));
        QTRY_VERIFY(fixture.visible("rrAnnotationRemove0"));
        QVERIFY(fixture.press("rrAnnotationRemove0"));
        QTRY_VERIFY(fixture.visible("rrKeepAnnotation"));
        const auto savedPassage = fixture.stored("highlights.json", "highlights").first().toObject().value("text").toString();
        auto *removeDetail = qobject_cast<QLabel *>(fixture.widget("rrRemoveAnnotationDetail"));
        QVERIFY(removeDetail);
        QVERIFY(removeDetail->text().contains(savedPassage.left(40)));
        QVERIFY(!removeDetail->text().contains("will be removed"));
        QVERIFY(fixture.press("rrKeepAnnotation"));
        QTRY_VERIFY(!fixture.visible("rrRemoveAnnotation"));
        QCOMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        QVERIFY(fixture.press("rrAnnotationRemove0"));
        QTRY_VERIFY(fixture.visible("rrConfirmRemoveAnnotation"));
        QVERIFY(fixture.press("rrConfirmRemoveAnnotation"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 0);
        QVERIFY(fixture.visible("rrBookmarkListEmpty"));
        QVERIFY(fixture.press("rrBookmarkListClose"));
        QVERIFY(fixture.press("rrBookmarks"));
        QVERIFY(bookmarksTab->isChecked());
        QTRY_VERIFY(fixture.visible("rrBookmark0"));
        QVERIFY(fixture.press("rrBookmarkRemove0"));
        QTRY_VERIFY(fixture.visible("rrConfirmRemoveBookmark"));
        QVERIFY(fixture.press("rrConfirmRemoveBookmark"));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 0);
        QVERIFY(fixture.press("rrBookmarkListClose"));
        QVERIFY(fixture.press("rrBookmarks"));
        QVERIFY(bookmarksTab->isChecked());
        QVERIFY(fixture.visible("rrBookmarkListEmpty"));
        const auto other = fixture.directory.path() + "/other-marks.epub";
        QVERIFY(createBook(fixture.directory.path(), "other-marks.epub"));
        int expectedOpens = opened.count();
        for (const auto &path : {other, book}) {
            ++expectedOpens;
            fixture.window.openBook(path);
            QTRY_COMPARE(opened.count(), expectedOpens);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(fixture.visible("rrBookmarks"));
            QVERIFY(fixture.press("rrBookmarks"));
            QVERIFY(fixture.press("rrMarksSort"));
            const auto expected = path == other ? "rrMarksSort_newest" : "rrMarksSort_oldest";
            QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget(expected))->isChecked());
            QVERIFY(fixture.press("rrMarksSort_oldest"));
            QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value("marks_sort").toString(), QString("oldest"));
        }
    }
    void links_data() { bookmarks_data(); }
    void links() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QVERIFY(writeBook(fixture.directory.path(), "links.epub", test::linkBook()));
        const auto path = fixture.directory.path() + "/links.epub";
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(path);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        auto *page = fixture.widget("rrPage");
        const auto origin = fixture.state().value("position");
        const auto note = fixture.linkPoint(path, "short note");
        QVERIFY(note.x() >= 0);
        QVERIFY(fixture.selectWord(page, note));
        QTRY_VERIFY(fixture.visible("rrSelectionLink"));
        QVERIFY(!fixture.visible("rrDictionary"));
        auto *lookup = fixture.widget("rrSelectionLookup"), *follow = fixture.widget("rrSelectionLink"),
             *clear = fixture.widget("rrAnnotationCancel");
        QCOMPARE(lookup->geometry().top(), follow->geometry().top());
        QVERIFY(!fixture.visible("rrSelectionAnnotate"));
        QVERIFY(lookup->geometry().left() < follow->geometry().left());
        QCOMPARE(lookup->geometry().top(), clear->geometry().top());
        QVERIFY(follow->geometry().left() < clear->geometry().left());
        QVERIFY(fixture.click(page, QPoint(10, page->height() / 2)));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        auto *reading = static_cast<ReadingPage *>(page);
        QVERIFY(reading->annotationHandleRect(true).isEmpty() && reading->annotationHandleRect(false).isEmpty());
        QCOMPARE(fixture.state().value("position"), origin);
        QVERIFY(fixture.selectWord(page, note));
        QTRY_VERIFY(fixture.visible("rrSelectionLookup"));
        QVERIFY(fixture.press("rrSelectionLookup"));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        auto *headword = qobject_cast<QLabel *>(fixture.widget("rrDictionaryWord"));
        QVERIFY(headword);
        QVERIFY(headword->text() == "short" || headword->text() == "note");
        QTRY_VERIFY(!reading->annotationHandleRect(true).isEmpty() && !reading->annotationHandleRect(false).isEmpty());
        QVERIFY(fixture.press("rrDictionaryClose"));
        QTRY_VERIFY(!reading->annotationContains(note - QPoint(0, reading->topHeight())));
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        // Annotation tools require enabling the mode in the reading menu.
        QVERIFY(fixture.startAnnotation(page, note));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(!fixture.visible("rrSelectionLink"));
        QVERIFY(fixture.press("rrAnnotationCancel"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QVERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(fixture.selectWord(page, note));
        QTRY_VERIFY(fixture.visible("rrSelectionLink"));
        QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());
        QVERIFY(fixture.press("rrSelectionLink"));
        QTRY_VERIFY(fixture.visible("rrLinkPanel"));
        QVERIFY(!fixture.widget("rrLinkActions"));
        QVERIFY(fixture.press("rrLinkClose"));
        QTRY_VERIFY(!fixture.visible("rrLinkPanel"));
        QCOMPARE(fixture.state().value("position"), origin);
        const auto passage = fixture.linkPoint(path, "later passage");
        QVERIFY(passage.x() >= 0);
        QVERIFY(fixture.hold(page, passage));
        QTRY_VERIFY(!fixture.state().value("busy").toBool() && fixture.state().value("position") != origin);
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrScrubberReturn"));
        QVERIFY(fixture.press("rrScrubberReturn"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("position"), origin);
        if (fixture.visible("rrControlsTop"))
            QVERIFY(fixture.press("rrPage"));
        const auto website = fixture.linkPoint(path, "website");
        QVERIFY(website.x() >= 0);
        QVERIFY(fixture.hold(page, website));
        QTRY_VERIFY(fixture.visible("rrBrowserConfirmation"));
        QVERIFY(!fixture.widget("rrLinkActions"));
        QCOMPARE(fixture.state().value("position"), origin);
        // Cancel through the actual reader. External browser dispatch is
        // checked separately by the component's controlled browser service.
        QVERIFY(fixture.press("rrLinkNo"));
        QTRY_VERIFY(!fixture.visible("rrBrowserConfirmation"));
        QCOMPARE(fixture.state().value("position"), origin);
    }
    void tappableLinks_data() { bookmarks_data(); }
    void tappableLinks() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QVERIFY(writeBook(fixture.directory.path(), "direct.epub", test::linkBook()));
        const auto path = fixture.directory.path() + "/direct.epub";
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(path);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        auto *page = fixture.widget("rrPage");
        const auto origin = fixture.state().value("position");
        QVERIFY(!storage::readJson(fixture.paths.preferences()).contains("tappable_links"));
        for (const auto *mode : {"off", "footnotes", "all"}) {
            if (!fixture.visible("rrControlsTop"))
                QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(fixture.visible("rrSettings"));
            QVERIFY(fixture.press("rrSettings"));
            QVERIFY(fixture.press("rrSettingsControls"));
            auto *linkRow = fixture.widget("rrTappableLinks");
            QVERIFY(linkRow);
            for (const auto *name : {"rrPageNavigation", "rrTapZones"}) {
                auto *neighbor = fixture.widget(name);
                QVERIFY(neighbor);
                QCOMPARE(linkRow->height(), neighbor->height());
                QCOMPARE(linkRow->font(), neighbor->font());
                QCOMPARE(linkRow->styleSheet(), neighbor->styleSheet());
            }
            QVERIFY(fixture.press("rrTappableLinks"));
            QTRY_VERIFY(fixture.visible("rrTappableLinks_off"));
            if (QString(mode) == "off") {
                QVERIFY(qobject_cast<QCheckBox *>(fixture.widget("rrTappableLinks_off"))->isChecked());
            }
            QVERIFY(fixture.press(qPrintable(QString("rrTappableLinks_") + mode)));
            QTRY_VERIFY(!fixture.visible("rrTappableLinksPopup"));
            // Disable page-turn taps so non-link taps can be tested without leaving this page.
            QVERIFY(fixture.press("rrPageNavigation"));
            QTRY_VERIFY(fixture.visible("rrNavigation_swipes_only"));
            QVERIFY(fixture.press("rrNavigation_swipes_only"));
            QVERIFY(fixture.press("rrSettingsClose"));
            QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
            QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value("tappable_links").toString(), QString(mode));
            if (fixture.visible("rrControlsTop"))
                QVERIFY(fixture.press("rrPage"));
            for (const auto *label : {"short note", "website"}) {
                const auto point = fixture.linkPoint(path, label);
                QVERIFY(point.x() >= 0);
                QVERIFY(fixture.click(page, point));
                const bool follows = QString(mode) == "all" || (QString(mode) == "footnotes" && QString(label) == "short note");
                if (follows) {
                    const bool note = QString(label) == "short note";
                    QTRY_VERIFY(fixture.visible(note ? "rrLinkPanel" : "rrBrowserConfirmation"));
                    QVERIFY(!fixture.visible("rrControlsTop"));
                    QVERIFY(fixture.press(note ? "rrLinkClose" : "rrLinkNo"));
                } else {
                    QVERIFY(!fixture.visible("rrLinkPanel"));
                    QVERIFY(!fixture.visible("rrBrowserConfirmation"));
                    if (fixture.visible("rrControlsTop"))
                        QVERIFY(fixture.press("rrPage"));
                }
                QCOMPARE(fixture.state().value("position"), origin);
            }
            const auto outside = fixture.coordinateQuery(path, QPoint(-1, -1), [top = static_cast<ReadingPage *>(page)->topHeight()](ReaderModel &coordinates) {
                for (const auto &link : coordinates.pageLinks())
                    if (coordinates.linkDetails(link.source).label == "short note") {
                        const auto r = link.rectangles.first();
                        return QPoint(r.left() - 2, r.center().y() + top);
                    }
                return QPoint(-1, -1);
            });
            QVERIFY(outside.x() >= 0);
            QVERIFY(fixture.click(page, outside));
            QVERIFY(!fixture.visible("rrLinkPanel"));
            QCOMPARE(fixture.state().value("position"), origin);
            if (fixture.visible("rrControlsTop"))
                QVERIFY(fixture.press("rrPage"));
            // A long press keeps the connected selection menu in every mode.
            const auto note = fixture.linkPoint(path, "short note");
            QVERIFY(fixture.selectWord(page, note));
            QTRY_VERIFY(fixture.visible("rrSelectionLink"));
            QVERIFY(fixture.press("rrAnnotationCancel"));
            // Manual reveal still includes ordinary links with the persistent option off or footnotes-only.
            QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(fixture.visible("rrScrubberLinks"));
            QVERIFY(fixture.press("rrScrubberLinks"));
            QVERIFY(fixture.click(page, fixture.linkPoint(path, "website")));
            QTRY_VERIFY(fixture.visible("rrBrowserConfirmation"));
            QVERIFY(fixture.press("rrLinkNo"));
            QCOMPARE(fixture.state().value("position"), origin);
        }
        // Direct links also win over normal tap zones, but annotation mode keeps its input.
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsControls"));
        QVERIFY(fixture.press("rrPageNavigation"));
        QTRY_VERIFY(fixture.visible("rrNavigation_taps_and_swipes"));
        QVERIFY(fixture.press("rrNavigation_taps_and_swipes"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        if (fixture.visible("rrControlsTop"))
            QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.click(page, fixture.linkPoint(path, "short note")));
        QTRY_VERIFY(fixture.visible("rrLinkPanel"));
        QCOMPARE(fixture.state().value("position"), origin);
        QVERIFY(fixture.press("rrLinkClose"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QVERIFY(fixture.click(page, fixture.linkPoint(path, "short note")));
        QVERIFY(!fixture.visible("rrLinkPanel"));
        if (!fixture.visible("rrControlsTop"))
            QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        // A fresh reader restores the global choice.
        QVERIFY(writeBook(fixture.directory.path(), "second.epub", test::linkBook()));
        fixture.window.openBook(fixture.directory.path() + "/second.epub");
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(fixture.click(fixture.widget("rrPage"), fixture.linkPoint(fixture.directory.path() + "/second.epub", "short note")));
        QTRY_VERIFY(fixture.visible("rrLinkPanel"));
    }
    void bookmarkCornerLinks_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("footnote");
        for (bool dark : {false, true})
            for (const auto *mode : {"off", "footnotes", "all"})
                for (bool footnote : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(dark ? "dark" : "light", mode,
                                                                     footnote ? "note" : "website")))
                        << dark << QString(mode) << footnote;
    }
    void bookmarkCornerLinks() {
        QFETCH(bool, dark);
        QFETCH(QString, mode);
        QFETCH(bool, footnote);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences["tappable_links"] = mode;
        preferences["page_margins"] = QJsonObject{{"margin_pixels", 24}, {"top_margin_pixels", 0},
                                                 {"bottom_margin_pixels", 24}};
        preferences["last_text_layout"] = QJsonObject{{"font_pixels", 36}, {"text_alignment", "publisher"}};
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        auto files = test::linkBook();
        files["OEBPS/first.xhtml"] =
            QByteArray("<html xmlns='http://www.w3.org/1999/xhtml' xmlns:epub='http://www.idpf.org/2007/ops'>"
                       "<head><title>Corner link</title></head><body><p style='text-align:right!important;margin:0'>") +
            (footnote ? "<a epub:type='noteref' href='Notes/foot%20notes.xhtml#short'>Corner link</a>"
                      : "<a href='https://example.org/book'>Corner link</a>") + "</p></body></html>";
        QVERIFY(writeBook(fixture.directory.path(), "corner.epub", files));
        const auto path = fixture.directory.path() + "/corner.epub";
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(path);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(page);
        const auto point = fixture.linkPoint(path, "Corner link");
        QVERIFY2(page->bookmarkCorner().contains(point), qPrintable(QString("Link at %1,%2 missed bookmark corner")
                                                                     .arg(point.x()).arg(point.y())));
        const auto origin = fixture.state().value("position");
        const bool follows = mode == "all" || (mode == "footnotes" && footnote);
        const auto *panel = footnote ? "rrLinkPanel" : "rrBrowserConfirmation";
        const auto *close = footnote ? "rrLinkClose" : "rrLinkNo";
        QVERIFY(fixture.click(page, point));
        if (follows) {
            QTRY_VERIFY(fixture.visible(panel));
            QCOMPARE(fixture.state().value("bookmark_count").toInt(), 0);
            QVERIFY(fixture.press(close));
            QTRY_VERIFY(!fixture.visible(panel));
        } else {
            QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
            QVERIFY(!fixture.visible(panel));
            QVERIFY(fixture.click(page, point));
            QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 0);
        }
        // The empty part of the corner still toggles a bookmark in every mode.
        const auto emptyCorner = page->bookmarkCorner().topRight() + QPoint(-ui::px(4), ui::px(4));
        QVERIFY(fixture.click(page, emptyCorner));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
        if (follows) {
            // Link priority must also preserve an existing bookmark.
            QVERIFY(fixture.click(page, point));
            QTRY_VERIFY(fixture.visible(panel));
            QCOMPARE(fixture.state().value("bookmark_count").toInt(), 1);
            QVERIFY(fixture.press(close));
            QTRY_VERIFY(!fixture.visible(panel));
        }
        QVERIFY(fixture.click(page, emptyCorner));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 0);
        QCOMPARE(fixture.state().value("position"), origin);
    }
    void settings_data() { bookmarks_data(); }
    void dictionaryAppearance_data() { bookmarks_data(); }
    void dictionaryAppearance() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *reader = fixture.window.readerWindow();
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto position = fixture.state().value("position");
        auto bodyFace = fixture.state().value("font_face").toString();
        if (bodyFace.isEmpty())
            bodyFace = fixture.state().value("text_font_face").toString();
        if (bodyFace.isEmpty())
            bodyFace = QString::fromLatin1(defaultFontFace);
        auto *definition = dynamic_cast<ui::PagedText *>(fixture.widget("rrDefinition"));
        QVERIFY(definition);
        QVERIFY(fixture.selectWord(page, fixture.wordPoint()));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QCOMPARE(definition->textFont().family(), bodyFace);
        QVERIFY(fixture.press("rrDictionaryClose"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsSystem"));
        QVERIFY(revealSettingsOption(fixture, "rrDictionaryFontSize"));
        QVERIFY(fixture.press("rrDictionaryFontSize"));
        QTRY_VERIFY(fixture.visible("rrDictionaryFontSizePopup"));
        auto *defaultPreview = fixture.widget("rrDictionaryFontPreview100");
        QVERIFY(defaultPreview);
        auto *defaultSample = defaultPreview->findChild<QLabel *>();
        QVERIFY(defaultSample);
        QCOMPARE(defaultSample->font().family(), bodyFace);
        QVERIFY(fixture.press("rrDictionaryFontSize100"));
        QVERIFY(revealSettingsOption(fixture, "rrDictionaryFont"));
        auto *fontOption = dynamic_cast<ui::CaptionButton *>(fixture.widget("rrDictionaryFont"));
        QVERIFY(fontOption);
        QCOMPARE(fontOption->typefacePreview(), bodyFace);
        QVERIFY(fixture.press("rrDictionaryFont"));
        QTRY_VERIFY(fixture.visible("rrSettingsFontPopup"));
        QAbstractButton *chosen = nullptr;
        for (int attempt = 0; attempt < 12 && !chosen; ++attempt) {
            for (auto *option : reader->findChildren<QAbstractButton *>())
                if (option->isVisible() && option->objectName().startsWith("rrSettingsFontChoice") &&
                    option->property("reader_font_family").toString() == "Libron")
                    chosen = option;
            if (!chosen)
                QVERIFY(fixture.press("rrSettingsFontNext"));
        }
        QVERIFY(chosen);
        QVERIFY(fixture.press(qPrintable(chosen->objectName())));
        QTRY_VERIFY(!fixture.visible("rrSettingsFontPopup"));
        QVERIFY(revealSettingsOption(fixture, "rrDictionaryFontSize"));
        QVERIFY(fixture.press("rrDictionaryFontSize"));
        QTRY_VERIFY(fixture.visible("rrDictionaryFontSizePopup"));
        auto *popup = fixture.widget("rrDictionaryFontSizePopup");
        for (int percent : appearance::dictionaryFontSizes) {
            auto *option = fixture.widget(qPrintable(QString("rrDictionaryFontSize%1").arg(percent)));
            QVERIFY(option && option->isVisible());
            QVERIFY(popup->rect().contains(QRect(option->mapTo(popup, QPoint()), option->size())));
        }
        QVERIFY(fixture.press("rrDictionaryFontSize140"));
        QVERIFY(!storage::readJson(fixture.paths.preferences()).contains("dictionary_font_face"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("dictionary_font_face").toString(), QString("Libron"));
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("dictionary_font_size").toInt(), 140);
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(fixture.selectWord(page, fixture.wordPoint()));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        definition = dynamic_cast<ui::PagedText *>(fixture.widget("rrDefinition"));
        QVERIFY(definition);
        QCOMPARE(definition->textFont().family(), QString("Libron"));
        QCOMPARE(definition->textFont().pixelSize(), DictionaryPanel::definitionFont("Libron", 140).pixelSize());
        QVERIFY(fixture.press("rrDictionaryClose"));
        reader->close();
        QTRY_VERIFY(fixture.window.idle());
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        definition = dynamic_cast<ui::PagedText *>(fixture.widget("rrDefinition"));
        QVERIFY(definition);
        QCOMPARE(definition->textFont().family(), QString("Libron"));
        QCOMPARE(definition->textFont().pixelSize(), DictionaryPanel::definitionFont("Libron", 140).pixelSize());
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsDefaults"));
        QVERIFY(fixture.press("rrRestoreDefaults"));
        QTRY_VERIFY(fixture.visible("rrRestoreConfirmation"));
        QVERIFY(fixture.press("rrConfirmDefaults"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto defaults = storage::readJson(fixture.paths.preferences());
        QVERIFY(defaults.value("dictionary_font_face").toString().isEmpty());
        QCOMPARE(defaults.value("dictionary_font_size").toInt(), 100);
    }
    void dictionaryRedraw_data() { bookmarks_data(); }
    void dictionaryRedraw() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(0);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrSettings"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsDictionaries"));
        QTRY_VERIFY(fixture.visible("rrDictionaryRow0"));
        auto *dictionaries = dynamic_cast<reader::DictionariesPage *>(fixture.widget("rrDictionaries"));
        QVERIFY(dictionaries);
        dictionaries->setStorage({}, 32000000000LL);
        QCoreApplication::processEvents();
        auto *row = fixture.widget("rrDictionaryRow0");
        QVERIFY(row);
        bool found = false;
        for (auto *label : row->findChildren<QLabel *>()) {
            if (!label->text().contains("English"))
                continue;
            found = true;
            QCOMPARE(label->palette().color(QPalette::WindowText), dark ? QColor(Qt::white) : QColor(Qt::black));
            auto *canvas = fixture.window.findChild<QGraphicsView *>();
            auto *stage = fixture.window.centralWidget();
            const auto at = [&](QPoint point) {
                return canvas->viewport()->mapTo(stage, canvas->mapFromScene(label->mapTo(fixture.window.readerWindow(), point)));
            };
            const QImage frame(fixture.window.saveScreenshot(fixture.directory.path()));
            QVERIFY(!frame.isNull());
            const qreal scale = frame.devicePixelRatio();
            const QRect bounds(at(label->rect().topLeft()) * scale, at(label->rect().bottomRight()) * scale);
            QVERIFY(frame.rect().contains(bounds));
            int ink = 0;
            const auto pixels = frame.convertToFormat(QImage::Format_ARGB32);
            for (int y = bounds.top() + 2; y < bounds.bottom() - 2; ++y)
                for (int x = bounds.left() + 2; x < bounds.right() - 2; ++x) {
                    const auto pixel = pixels.pixel(x, y);
                    const int gray = qGray(pixel);
                    ink += qAlpha(pixel) > 200 && (dark ? gray > 200 : gray < 80);
                }
            QVERIFY2(ink > 10, "Dictionary title disappeared from the actual canvas capture after rebuilding the list");
        }
        QVERIFY(found);
    }
    void settings() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(0);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrSettings"));
        QVERIFY(fixture.press("rrSettings"));
        struct Option { const char *tab, *name, *key; };
        QJsonObject expected;
        for (const auto &option : {Option{"rrSettingsControls", "rrShowLinkIndicator", "show_link_indicator"},
                                   Option{"rrSettingsDefaults", "rrInheritTextLayout", "inherit_text_layout"},
                                   Option{"rrSettingsUI", "rrHideStatusBar", "native_hide_status_bar"}}) {
            QVERIFY(fixture.press(option.tab));
            QTRY_VERIFY(fixture.visible(option.name));
            auto *box = fixture.window.readerWindow()->findChild<QCheckBox *>(option.name);
            QVERIFY(box);
            const bool previous = box->isChecked();
            QVERIFY(fixture.press(option.name));
            QTRY_COMPARE(box->isChecked(), !previous);
            expected[option.key] = !previous;
        }
        // Settings are a draft until the panel closes.
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        for (auto it = expected.begin(); it != expected.end(); ++it)
            QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value(it.key()), it.value());
        const auto saved = storage::readJson(fixture.paths.preferences());
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsControls"));
        QTRY_VERIFY(fixture.visible("rrShowLinkIndicator"));
        QCOMPARE(fixture.window.readerWindow()->findChild<QCheckBox *>("rrShowLinkIndicator")->isChecked(),
                 saved.value("show_link_indicator").toBool());
        QVERIFY(fixture.press("rrSettingsDefaults"));
        QTRY_VERIFY(fixture.visible("rrRestoreDefaults"));
        QVERIFY(fixture.press("rrRestoreDefaults"));
        QTRY_VERIFY(fixture.visible("rrRestoreConfirmation"));
        QVERIFY(fixture.press("rrKeepDefaults"));
        QTRY_VERIFY(!fixture.visible("rrRestoreConfirmation"));
        QCOMPARE(storage::readJson(fixture.paths.preferences()), saved);
        QVERIFY(fixture.press("rrSettingsControls"));
        QTRY_VERIFY(fixture.visible("rrShowLinkIndicator"));
        auto *indicator = fixture.window.readerWindow()->findChild<QCheckBox *>("rrShowLinkIndicator");
        const bool previous = indicator->isChecked();
        {
            BlockedFile blocked(fixture.paths.preferences());
            QVERIFY(blocked.active());
            QVERIFY(fixture.press("rrShowLinkIndicator"));
            QCOMPARE(indicator->isChecked(), !previous);
            QVERIFY(fixture.press("rrSettingsClose"));
            QTRY_VERIFY(fixture.visible("rrSettingsWriteError"));
            QVERIFY(blocked.restore());
        }
        QCOMPARE(storage::readJson(fixture.paths.preferences()), saved);
        QVERIFY(fixture.press("rrSettingsWriteErrorConfirm"));
        QTRY_VERIFY(!fixture.visible("rrSettingsWriteError"));
        QTRY_VERIFY(fixture.visible("rrGeneralSettings"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value("show_link_indicator").toBool(), !previous);
    }
    void advancedText_data() {
        QTest::addColumn<int>("dpi");
        QTest::addColumn<QString>("bookFont");
        QTest::addColumn<QString>("alignment");
        QTest::newRow("300dpi") << 300 << QString("NV Literata") << QString("left");
        QTest::newRow("native") << 0 << QString("NV Literata") << QString("justify");
        QTest::newRow("publisher-300dpi") << 300 << QString() << QString("publisher");
        QTest::newRow("publisher-native") << 0 << QString() << QString("right");
    }
    void advancedText() {
        QFETCH(int, dpi);
        QFETCH(QString, bookFont);
        QFETCH(QString, alignment);
        Fixture fixture;
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences["last_text_layout"] = QJsonObject{{"font_face", bookFont}, {"text_alignment", alignment}};
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        auto *resolution = fixture.window.findChild<QComboBox *>("desktopResolutionPicker");
        QVERIFY(resolution);
        resolution->setCurrentIndex(resolution->findData(dpi));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("font_face").toString(), bookFont);
        const auto sampleFace = bookFont.isEmpty() ? fixture.state().value("text_font_face").toString() : bookFont;
        QVERIFY(!sampleFace.isEmpty());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrSettings"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsStyling"));
        QTRY_VERIFY(fixture.visible("rrSettingsClose"));
        QVERIFY(fixture.visible("rrSettingsTitle"));
        QCOMPARE(fixture.widget("rrSettingsTitle")->font().family(),
                 ui::brandFont(ui::metrics::chrome::titleText).family());
        QCOMPARE(fixture.widget("rrSettingsTitle")->font().pixelSize(), int(ui::metrics::chrome::titleText));
        for (const auto *name : {"rrReturn", "rrSettings", "rrSearchOpen", "rrBookmarks", "rrAnnotate"})
            QVERIFY2(!fixture.visible(name), name);
        auto *continuation = qobject_cast<QLabel *>(fixture.widget("rrSettingsOptionsContinuationHint"));
        QVERIFY(continuation);
        QTRY_VERIFY(continuation->isVisible());
        QVERIFY(continuation->font().italic());
        QVERIFY(fixture.press("rrSettingsOptionsNext"));
        QTRY_VERIFY(!continuation->isVisible());
        QVERIFY(fixture.press("rrSettingsOptionsPrevious"));
        QTRY_VERIFY(continuation->isVisible());
        const auto reveal = [&fixture](const char *name) {
            for (int page = 0; page < 8; ++page) {
                auto *row = fixture.widget(name);
                if (row && row->visibleRegion().contains(row->rect().center()))
                    return true;
                if (!fixture.press("rrSettingsOptionsNext"))
                    return false;
                QCoreApplication::processEvents();
            }
            return false;
        };
        QVERIFY(reveal("rrWordSpacing"));
        auto *wordSpacing = dynamic_cast<ui::CaptionButton *>(fixture.widget("rrWordSpacing"));
        QVERIFY(wordSpacing);
        QCOMPARE(wordSpacing->caption(), strings::get("settings.word_spacing_normal").toUpper());
        QVERIFY(fixture.press("rrWordSpacing"));
        QTRY_VERIFY(fixture.visible("rrWordSpacing_wide"));
        QCOMPARE(ui::registerPrivateInterfaceFont(QStringLiteral(KOBALT_TEST_ASSETS) + "/fonts/InstrumentSerif-Regular.ttf",
                                                  "Sample Private Serif", true), ui::PrivateFontRegistration::Private);
        // Force the private renderer used by fonts that Qt cannot load.
        // Words with descenders must share the capital's baseline.
        const auto verifySample = [](QWidget *preview, int tracking, int spacing) {
            const auto savedFont = preview->font();
            const auto savedStyle = preview->styleSheet();
            const auto savedSize = preview->size();
            preview->setStyleSheet("font-family: 'Sample Private Serif';");
            auto font = preview->font();
            font.setPixelSize(32);
            font.setLetterSpacing(QFont::AbsoluteSpacing, 32 * tracking / 100.0);
            font.setWordSpacing(0);
            QVERIFY(ui::needsPrivateText("A quiet evening", font));
            const QString text = "A quiet evening";
            font.setWordSpacing(spacing > 0
                ? ui::interfaceTextWidth(" ", font) * (spacing / 100.0 - 1)
                : (312 - ui::interfaceTextWidth(text, font)) / 2.0);
            preview->resize(360, 150);
            const auto image = preview->grab().toImage().scaled(360, 150);
            int baseline = -1;
            for (const auto &word : {QString("A"), QString("quiet"), QString("evening")}) {
                const int offset = text.indexOf(word);
                const int x = 24 + (312 - ui::interfaceTextWidth(text, font)) / 2 +
                              (offset ? ui::measurePrivateText(text.left(offset), font) +
                                            qRound(font.letterSpacing()) : 0);
                const auto glyphs = ui::renderPrivateText(word, font);
                QRect actualInk, expectedInk;
                for (int y = 24; y < 68; ++y)
                    for (int column = x; column < x + glyphs.advance; ++column)
                        if (qGray(image.pixel(column, y)) < 128)
                            actualInk = actualInk.united(QRect(column, y, 1, 1));
                for (int y = 0; y < glyphs.image.height(); ++y)
                    for (int column = 0; column < glyphs.image.width(); ++column)
                        if (qAlpha(glyphs.image.pixel(column, y)) >= 128)
                            expectedInk = expectedInk.united(QRect(column, y, 1, 1));
                QVERIFY(!actualInk.isEmpty() && !expectedInk.isEmpty());
                QVERIFY(qAbs(actualInk.width() - expectedInk.width()) <= 1);
                const int wordBaseline = actualInk.bottom() - glyphs.origin.y() - expectedInk.bottom();
                if (baseline >= 0)
                    QVERIFY2(qAbs(wordBaseline - baseline) <= 1, "Sample words do not share a baseline");
                baseline = wordBaseline;
            }
            // Compare the first glyph with a direct raster at the output
            // font size. Scaling an already rendered sample must fail this.
            preview->resize(320, 134);
            for (qreal scale : {.73, 1.0, 1.37}) {
                for (qreal dpr : {1.0, 2.0}) {
                    QImage output(qCeil((preview->width() * scale + 4) * dpr),
                                  qCeil((preview->height() * scale + 4) * dpr), QImage::Format_ARGB32);
                    output.setDevicePixelRatio(dpr);
                    output.fill(Qt::white);
                    QRect pixels;
                    {
                        QPainter painter(&output);
                        painter.translate(.33, .47);
                        painter.scale(scale, scale);
                        pixels = painter.deviceTransform().mapRect(QRectF(preview->rect())).toAlignedRect();
                        preview->render(&painter);
                    }
                    auto pixelFont = font;
                    pixelFont.setPixelSize(qRound(pixels.height() * 32 / 150.0));
                    pixelFont.setLetterSpacing(QFont::AbsoluteSpacing, pixelFont.pixelSize() * tracking / 100.0);
                    pixelFont.setWordSpacing(0);
                    const int lineWidth = qRound(pixels.width() * 312 / 360.0);
                    pixelFont.setWordSpacing(spacing > 0
                        ? ui::interfaceTextWidth(" ", pixelFont) * (spacing / 100.0 - 1)
                        : (lineWidth - ui::interfaceTextWidth(text, pixelFont)) / 2.0);
                    const auto glyph = ui::renderPrivateText("A", pixelFont);
                    const int left = pixels.left() + qRound(pixels.width() * 24 / 360.0) +
                                     (lineWidth - ui::interfaceTextWidth(text, pixelFont)) / 2;
                    const int top = pixels.top() + qRound(pixels.height() * 24 / 150.0);
                    const int lineHeight = qRound(pixels.height() * 44 / 150.0);
                    QRect actualInk, expectedInk;
                    for (int y = top; y < top + lineHeight; ++y)
                        for (int x = left; x < left + glyph.image.width() + 2; ++x)
                            if (qGray(output.pixel(x, y)) < 128)
                                actualInk = actualInk.united(QRect(x, y, 1, 1));
                    for (int y = 0; y < glyph.image.height(); ++y)
                        for (int x = 0; x < glyph.image.width(); ++x)
                            if (qAlpha(glyph.image.pixel(x, y)) >= 128)
                                expectedInk = expectedInk.united(QRect(x, y, 1, 1));
                    QRect lineInk;
                    for (int y = top; y < top + lineHeight; ++y)
                        for (int x = pixels.left() + 2; x < pixels.right() - 1; ++x)
                            if (qGray(output.pixel(x, y)) < 128)
                                lineInk = lineInk.united(QRect(x, y, 1, 1));
                    QVERIFY(!lineInk.isEmpty());
                    QVERIFY2(qAbs(lineInk.center().x() - pixels.center().x()) <= 2,
                             "Sample text is not centered in its box");
                    QCOMPARE(actualInk.size(), expectedInk.size());
                    for (int y = 0; y < actualInk.height(); ++y)
                        for (int x = 0; x < actualInk.width(); ++x)
                            QVERIFY2(qAbs(qGray(output.pixel(actualInk.topLeft() + QPoint(x, y))) -
                                          (255 - qAlpha(glyph.image.pixel(expectedInk.topLeft() + QPoint(x, y))))) <= 1,
                                     "Sample glyph was resampled instead of rasterized at output resolution");
                }
            }
            preview->resize(savedSize);
            preview->setStyleSheet(savedStyle);
            preview->setFont(savedFont);
        };
        QImage previousSpaces;
        for (const auto *name : {"rrWordSpacingPreview_narrow", "rrWordSpacingPreview_normal", "rrWordSpacingPreview_wide"}) {
            auto *preview = fixture.widget(name);
            QVERIFY(preview && preview->isVisible());
            QVERIFY(preview->visibleRegion().contains(preview->rect()));
            auto *defaultFont = dynamic_cast<ui::CaptionButton *>(fixture.widget("rrDefaultFont"));
            QVERIFY(defaultFont);
            QCOMPARE(preview->font().family(), sampleFace);
            if (!bookFont.isEmpty())
                QVERIFY(preview->font().family() != defaultFont->typefacePreview());
            const auto image = preview->grab().toImage();
            QVERIFY(!image.isNull() && image != previousSpaces);
            previousSpaces = image;
            verifySample(preview, 0, QString(name).endsWith("narrow") ? 80 : QString(name).endsWith("wide") ? 120 : 100);
        }
        QVERIFY(fixture.click(fixture.widget("rrWordSpacingPreview_wide")));
        QTRY_VERIFY(!fixture.visible("rrWordSpacingPopup"));
        QCOMPARE(wordSpacing->caption(), strings::get("settings.word_spacing_wide").toUpper());
        QVERIFY(!storage::readJson(fixture.paths.preferences()).contains("word_spacing"));
        QVERIFY(reveal("rrJustificationTracking"));
        auto *tracking = dynamic_cast<ui::CaptionButton *>(fixture.widget("rrJustificationTracking"));
        auto *widows = dynamic_cast<ui::CaptionButton *>(fixture.widget("rrWidowsOrphans"));
        QVERIFY(tracking && widows);
        QCOMPARE(tracking->accessibleDescription(), strings::get("settings.justification_tracking_summary"));
        QCOMPARE(widows->accessibleDescription(), strings::get("settings.widows_orphans_summary"));
        QVERIFY(tracking->height() >= tracking->heightForWidth(tracking->width()));
        const auto *fontsHeading = fixture.widget("rrFontsFamiliesSection");
        const auto *advancedHeading = fixture.widget("rrAdvancedTextSection");
        const auto *contentHeading = fixture.widget("rrAdvancedContentSection");
        QVERIFY(fontsHeading && advancedHeading && contentHeading);
        QVERIFY(fontsHeading->y() < advancedHeading->y() && advancedHeading->y() < contentHeading->y());
        QCOMPARE(tracking->caption(), strings::get("settings.justification_tracking_off").toUpper());
        QCOMPARE(widows->caption(), strings::get("settings.widows_orphans_publisher").toUpper());
        QVERIFY(fixture.press("rrJustificationTracking"));
        QTRY_VERIFY(fixture.visible("rrTracking_more"));
        auto *notice = qobject_cast<QLabel *>(fixture.widget("rrTrackingAlignmentNotice"));
        QVERIFY(notice && notice->isVisible());
        QVERIFY(notice->font().italic());
        QCOMPARE(notice->palette().color(QPalette::WindowText), ui::noteColor(notice));
        const auto suffix = alignment == "left" ? "left" : alignment == "justify" ? "justified"
                            : alignment == "publisher" ? "publisher" : "other";
        QCOMPARE(notice->text(), strings::get((QString("settings.justification_tracking_current_") + suffix).toUtf8().constData()));
        QVERIFY(notice->visibleRegion().contains(notice->rect()));
        auto *footer = fixture.widget("rrTrackingAlignmentFooter");
        QVERIFY(footer);
        QVERIFY(notice->mapTo(footer, QPoint()).x() >= int(ui::metrics::chrome::inset));
        QVERIFY(notice->mapTo(footer, QPoint()).y() >= int(ui::metrics::spacing::row));
        auto *hint = fixture.widget("rrTrackingPopup")->findChild<QLabel *>("rrAdvancedTextHint");
        QVERIFY(hint && hint->textFormat() == Qt::RichText);
        QCOMPARE(hint->font().pixelSize(), int(ui::metrics::chrome::descriptionText + ui::Px{1}));
        QVERIFY(hint->text().contains("data:image/png;base64,"));
        QVERIFY(hint->visibleRegion().contains(hint->rect()));
        QImage previousPreview;
        for (const auto *name : {"rrTrackingPreview_off", "rrTrackingPreview_subtle", "rrTrackingPreview_more"}) {
            auto *preview = fixture.widget(name);
            QVERIFY(preview && preview->isVisible());
            auto *defaultFont = dynamic_cast<ui::CaptionButton *>(fixture.widget("rrDefaultFont"));
            QVERIFY(defaultFont);
            QCOMPARE(preview->font().family(), sampleFace);
            if (!bookFont.isEmpty())
                QVERIFY(preview->font().family() != defaultFont->typefacePreview());
            QVERIFY(preview->visibleRegion().contains(preview->rect()));
            const auto image = preview->grab().toImage();
            QVERIFY(!image.isNull() && image != previousPreview);
            previousPreview = image;
            verifySample(preview, QString(name).endsWith("more") ? 15 : QString(name).endsWith("subtle") ? 5 : 0, 0);
        }
        QVERIFY(fixture.click(fixture.widget("rrTrackingPreview_more")));
        QTRY_VERIFY(!fixture.visible("rrTrackingPopup"));
        QCOMPARE(tracking->caption(), strings::get("settings.justification_tracking_more").toUpper());
        QVERIFY(reveal("rrWidowsOrphans"));
        QVERIFY(fixture.press("rrWidowsOrphans"));
        QTRY_VERIFY(fixture.visible("rrWidowsOrphans_avoid"));
        auto *example = fixture.widget("rrPaginationPreview_avoid");
        QVERIFY(example && example->isVisible());
        QVERIFY(example->visibleRegion().contains(example->rect()));
        QVERIFY(fixture.widget("rrWidowsOrphansPopup")->rect().contains(
            QRect(example->mapTo(fixture.widget("rrWidowsOrphansPopup"), QPoint()), example->size())));
        // The native canvas paints controls at fractional screen scales.
        // All four dark paragraph bars must retain one output-pixel height.
        for (qreal scale : {.73, 1.11, 1.37}) {
            for (qreal dpr : {1.0, 2.0}) {
                QImage image(qCeil((example->width() * scale + 4) * dpr),
                             qCeil((example->height() * scale + 4) * dpr), QImage::Format_ARGB32);
                image.setDevicePixelRatio(dpr);
                image.fill(Qt::white);
                {
                    QPainter painter(&image);
                    painter.translate(.33, .47);
                    painter.scale(scale, scale);
                    example->render(&painter);
                }
                QVector<int> heights;
                bool inLine = false;
                for (int y = 0; y < image.height(); ++y) {
                    int ink = 0;
                    for (int x = 0; x < image.width(); ++x)
                        ink += qGray(image.pixel(x, y)) < 50;
                    if (ink > 10) {
                        if (!inLine)
                            heights.append(0);
                        ++heights.last();
                        inLine = true;
                    } else {
                        inLine = false;
                    }
                }
                QCOMPARE(heights.size(), 4);
                QCOMPARE(heights[0], heights[1]);
                QCOMPARE(heights[1], heights[2]);
                QCOMPARE(heights[2], heights[3]);
            }
        }
        QVERIFY(fixture.click(example));
        QTRY_VERIFY(!fixture.visible("rrWidowsOrphansPopup"));
        QVERIFY(!storage::readJson(fixture.paths.preferences()).contains("justification_tracking"));
        auto *closeSettings = qobject_cast<QPushButton *>(fixture.widget("rrSettingsClose"));
        QVERIFY(closeSettings);
        QCOMPARE(closeSettings->accessibleName(), strings::get("settings.close"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(!fixture.window.idle());
        QVERIFY(fixture.visible("rrReturn"));
        QVERIFY(fixture.visible("rrSettings"));
        QVERIFY(!fixture.visible("rrSettingsClose"));
        // Closing Settings and exiting the reader share the leftmost cell.
        // Ignore the second half of a double tap after the header changes.
        QVERIFY(fixture.press("rrReturn"));
        QVERIFY(!fixture.window.idle());
        QTest::qWait(ui::metrics::contents::exitGuardMilliseconds + 10);
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("word_spacing").toInt(), 120);
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("justification_tracking").toInt(), 15);
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("widows_orphans").toString(), "avoid");
        QTRY_COMPARE(fixture.state().value("word_spacing").toInt(), 120);
        QTRY_COMPARE(fixture.state().value("justification_tracking").toInt(), 15);
        QTRY_COMPARE(fixture.state().value("widows_orphans").toString(), "avoid");
        if (!fixture.visible("rrReturn"))
            QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrReturn"));
        QVERIFY(fixture.press("rrReturn"));
        QTRY_VERIFY(fixture.window.idle());
        opened.clear();
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("word_spacing").toInt(), 120);
        QTRY_COMPARE(fixture.state().value("justification_tracking").toInt(), 15);
        QTRY_COMPARE(fixture.state().value("widows_orphans").toString(), "avoid");
    }
    void fullWidthPanels_data() { bookmarks_data(); }
    void fullWidthPanels() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.resize(2200, 1000);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(0);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrNavChapters"));
        QVERIFY(fixture.press("rrNavChapters"));
        QTRY_VERIFY(fixture.visible("rrChapter1"));
        QVERIFY(fixture.press("rrChapter1"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto checkBounds = [&](QWidget *panel) {
            const auto *reader = fixture.window.readerWindow();
            QCOMPARE(panel->width(), reader->width());
            QCOMPARE(panel->x(), 0);
        };
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrNavLayout"));
        checkBounds(fixture.widget("rrControlsTop"));
        checkBounds(fixture.widget("rrBottomNavigation"));
        QVERIFY(fixture.press("rrNavLayout"));
        QTRY_VERIFY(fixture.visible("rrMargins"));
        checkBounds(fixture.widget("rrMenu"));
        const auto output = qEnvironmentVariable("KOBALT_TEST_CAPTURE_DIR");
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            QCoreApplication::processEvents();
            QVERIFY(fixture.window.grab().save(output + (dark ? "/full-width-layout-dark.png" : "/full-width-layout-light.png")));
        }
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.visible("rrNavLayout"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrSettings"));
        QVERIFY(fixture.press("rrSettings"));
        QTRY_VERIFY(fixture.visible("rrGeneralSettings"));
        checkBounds(fixture.widget("rrGeneralSettings"));
        if (!output.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(fixture.window.grab().save(output + (dark ? "/full-width-settings-dark.png" : "/full-width-settings-light.png")));
        }
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        if (!fixture.visible("rrNavLayout"))
            QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrNavLayout"));
        auto *reader = fixture.window.readerWindow();
        QCOMPARE(fixture.widget("rrControlsTop")->width(), reader->width());
        QCOMPARE(fixture.widget("rrBottomNavigation")->width(), reader->width());
        reader->resize(720, 1000);
        QTRY_COMPARE(fixture.widget("rrControlsTop")->width(), 720);
        QTRY_COMPARE(fixture.widget("rrBottomNavigation")->width(), 720);
    }
    void globalLayout() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(2);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavText"));
        QTRY_VERIFY(fixture.visible("rrLarger"));
        QVERIFY(fixture.press("rrLarger"));
        QTRY_VERIFY(fixture.state().value("preview_ready").toBool() && !fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrNavLayout"));
        QTRY_VERIFY(fixture.visible("rrWider"));
        QVERIFY(fixture.press("rrWider"));
        QTRY_VERIFY(!fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrVerticalMarginsMore"));
        QTRY_VERIFY(!fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrOrientation2"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool() && !fixture.visible("rrMargins"));
        const auto first = fixture.state();
        const auto settings = storage::readJson(fixture.paths.preferences());
        QCOMPARE(settings.value("orientation").toInt(), 2);
        const auto margins = settings.value("page_margins").toObject();
        QCOMPARE(margins.value("margin_pixels").toInt(), marginDesign(first.value("margin_pixels").toInt()));
        QCOMPARE(margins.value("top_margin_pixels").toInt(), marginDesign(first.value("top_margin_pixels").toInt()));
        QVERIFY(!settings.value("last_text_layout").toObject().contains("margin_pixels"));
        const auto firstSaved = storage::readBookState(fixture.paths, first.value("book").toString()).object;
        QVERIFY(!firstSaved.contains("margin_pixels") && !firstSaved.contains("orientation"));
        const auto other = fixture.directory.path() + "/other-layout.epub";
        QVERIFY(QFile::copy(book, other));
        const auto identity = QUrl::fromLocalFile(other).toString();
        QVERIFY(storage::writeJson(fixture.paths.bookState(identity),
                                  {{"book", identity}, {"fingerprint", first.value("fingerprint")},
                                   {"font_pixels", 34}, {"margin_pixels", 3}, {"top_margin_pixels", 6},
                                   {"bottom_margin_pixels", 9}, {"orientation", 1}}));
        fixture.window.openBook(other);
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto second = fixture.state();
        QCOMPARE(second.value("font_pixels").toInt(), 34);
        QCOMPARE(second.value("margin_pixels"), first.value("margin_pixels"));
        QCOMPARE(second.value("top_margin_pixels"), first.value("top_margin_pixels"));
        QCOMPARE(second.value("bottom_margin_pixels"), first.value("bottom_margin_pixels"));
        QCOMPARE(second.value("orientation").toInt(), 2);
        fixture.window.close();
        QTRY_VERIFY(fixture.window.idle());
        desktop::Window relaunched(fixture.paths, fixture.fontInputs, placeholderFontFamily(*assets, *copies));
        relaunched.resize(920, 800);
        relaunched.show();
        QSignalSpy reopened(&relaunched, &desktop::Window::bookOpened);
        relaunched.openBook(book);
        QTRY_COMPARE(reopened.count(), 1);
        QTRY_VERIFY(relaunched.readerWindow());
        const auto state = [&] {
            relaunched.readerWindow()->checkpoint();
            return storage::readJson(fixture.paths.stateReport);
        };
        QTRY_VERIFY(!state().value("busy").toBool());
        QCOMPARE(state().value("font_pixels"), first.value("font_pixels"));
        QCOMPARE(state().value("margin_pixels"), first.value("margin_pixels"));
        QCOMPARE(state().value("orientation").toInt(), 2);
        relaunched.close();
        QTRY_VERIFY(relaunched.idle());
    }
    void columns_data() { bookmarks_data(); }
    void columns() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(2);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        const auto reveal = [&fixture](const char *name) {
            for (int page = 0; page < 8; ++page) {
                auto *row = fixture.widget(name);
                if (row && row->visibleRegion().contains(row->rect().center()))
                    return true;
                if (!fixture.press("rrSettingsOptionsNext"))
                    return false;
                QCoreApplication::processEvents();
            }
            return false;
        };
        const auto capture = [&fixture, dark](const QString &name) {
            const auto output = qEnvironmentVariable("KOBALT_TEST_CAPTURE_DIR");
            if (output.isEmpty()) return true;
            QCoreApplication::processEvents();
            return QDir().mkpath(output) && fixture.window.grab().save(output + "/" + name + (dark ? "-dark.png" : "-light.png"));
        };
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavLayout"));
        QVERIFY(!fixture.widget("rrColumns1"));
        QVERIFY(!fixture.widget("rrColumnSpacing"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.visible("rrNavLayout"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(reveal("rrLandscapeColumns"));
        QVERIFY(reveal("rrColumnSpacingSetting"));
        QVERIFY(!fixture.widget("rrColumnSpacingSetting")->isEnabled());
        QVERIFY(capture("columns-disabled"));
        QVERIFY(fixture.widget("rrDisplayColumnsSection"));
        QVERIFY(fixture.press("rrLandscapeColumns"));
        QVERIFY(fixture.widget("rrColumnSpacingSetting")->isEnabled());
        QVERIFY(reveal("rrColumnSpacingSetting"));
        QVERIFY(fixture.press("rrColumnSpacingSetting"));
        QTRY_VERIFY(fixture.visible("rrColumnSpacing200"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrColumnSpacing150"))->isChecked());
        QCOMPARE(qobject_cast<QAbstractButton *>(fixture.widget("rrColumnSpacing150"))->text(), strings::get("settings.column_spacing_default"));
        QVERIFY(capture("column-spacing-choices"));
        QVERIFY(fixture.press("rrColumnSpacing200"));
        QVERIFY(capture("landscape-column-settings"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("columns").toInt(), 1);
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("two_columns_landscape").toBool(), true);
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("column_spacing_percent").toInt(), 200);
        auto *rotate = fixture.window.findChild<QAction *>("desktopRotateDevice");
        QVERIFY(rotate);
        const auto portrait = fixture.window.readerWindow()->size();
        rotate->trigger();
        QTRY_COMPARE(fixture.window.readerWindow()->size(), portrait.transposed());
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("columns").toInt(), 2);
        QCOMPARE(fixture.state().value("column_spacing").toInt(), fixture.state().value("font_pixels").toInt() * 2);
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.visible("rrNavLayout"));
        QVERIFY(capture("columns-landscape"));
        // A short landscape list can leave enough space for a hint on its
        // final page. That space must stay empty on both reader sizes.
        for (int screen : {1, 2}) {
            fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(screen);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            if (!fixture.visible("rrSettings"))
                QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrSettings"));
            auto *hint = fixture.widget("rrSettingsOptionsContinuationHint");
            auto *next = fixture.widget("rrSettingsOptionsNext");
            auto *previous = fixture.widget("rrSettingsOptionsPrevious");
            QVERIFY(hint && next && previous);
            for (int page = 0; page < 8 && previous->isEnabled(); ++page)
                QVERIFY(fixture.press("rrSettingsOptionsPrevious"));
            QTRY_VERIFY(hint->isVisible());
            for (int page = 0; page < 8 && next->isEnabled(); ++page)
                QVERIFY(fixture.press("rrSettingsOptionsNext"));
            QVERIFY(!next->isEnabled());
            QTRY_VERIFY(!hint->isVisible());
            QVERIFY(capture(QString("display-last-landscape-%1").arg(screen)));
            QVERIFY(fixture.press("rrSettingsOptionsPrevious"));
            QTRY_VERIFY(hint->isVisible());
            QVERIFY(fixture.press("rrSettingsOptionsNext"));
            QTRY_VERIFY(!hint->isVisible());
            QVERIFY(fixture.press("rrSettingsClose"));
        }
        rotate->trigger();
        QTRY_COMPARE(fixture.window.readerWindow()->size(), portrait);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("columns").toInt(), 1);
        if (!fixture.visible("rrReturn"))
            QVERIFY(fixture.press("rrPage"));
        // Settings guards Exit for the release of its Close tap.
        QTest::qWait(ui::metrics::contents::exitGuardMilliseconds + 20);
        QVERIFY(fixture.press("rrReturn"));
        QTRY_VERIFY(fixture.window.idle());
        QVERIFY(createBook(fixture.directory.path(), "other-columns.epub"));
        fixture.window.openBook(fixture.directory.path() + "/other-columns.epub");
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("columns").toInt(), 1);
        QCOMPARE(fixture.state().value("column_spacing_percent").toInt(), 200);
        rotate->trigger();
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("columns").toInt(), 2);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(reveal("rrLandscapeColumns"));
        QVERIFY(fixture.press("rrLandscapeColumns"));
        QVERIFY(reveal("rrColumnSpacingSetting"));
        QVERIFY(!fixture.widget("rrColumnSpacingSetting")->isEnabled());
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("columns").toInt(), 1);
        if (!fixture.visible("rrReturn"))
            QVERIFY(fixture.press("rrPage"));
        // Settings guards Exit for the release of its Close tap.
        QTest::qWait(ui::metrics::contents::exitGuardMilliseconds + 20);
        QVERIFY(fixture.press("rrReturn"));
        QTRY_VERIFY(fixture.window.idle());
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 3);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("columns").toInt(), 1);
    }
    void opticalSize() {
        Fixture fixture(false, true);
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrNavText"));
        QVERIFY(fixture.press("rrNavText"));
        QTRY_VERIFY(fixture.visible("rrFont"));
        QVERIFY(fixture.press("rrFont"));
        QWidget *choice = nullptr;
        for (int page = 0; page < 20 && !choice; ++page) {
            QCoreApplication::processEvents();
            for (auto *button : fixture.window.readerWindow()->findChildren<QPushButton *>())
                if (button->isVisible() && button->objectName().startsWith("rrFontChoice") &&
                    button->accessibleName().contains("Amstelvar", Qt::CaseInsensitive))
                    choice = button;
            if (!choice) {
                QVERIFY(fixture.widget("rrFontPagerNext")->isEnabled());
                QVERIFY(fixture.press("rrFontPagerNext"));
            }
        }
        QVERIFY(choice);
        QVERIFY(fixture.click(choice));
        QVERIFY(fixture.press("rrFontClose"));
        QTRY_VERIFY(fixture.visible("rrOpticalSizeMore"));
        QTRY_VERIFY(fixture.state().value("preview_ready").toBool());
        QTRY_VERIFY(fixture.state().value("preview_settings").toObject().value("font_face").toString().contains("Amstelvar"));
        QVERIFY(fixture.press("rrOpticalSizeMore"));
        QTRY_COMPARE(fixture.state().value("preview_settings").toObject().value("font_optical_size").toInt(), 15);
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_VERIFY(fixture.state().value("font_face").toString().contains("Amstelvar", Qt::CaseInsensitive));
        const auto selected = fixture.state();
        QCOMPARE(selected.value("font_optical_size").toInt(), 15); // Font declares a 14 pt default.
        QCOMPARE(selected.value("font_pixels").toInt(), defaultFontPixels());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrReturn"));
        QVERIFY(fixture.press("rrReturn"));
        QTRY_VERIFY(fixture.window.idle());
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("font_optical_size"), selected.value("font_optical_size"));
        QCOMPARE(fixture.state().value("font_face"), selected.value("font_face"));
    }
    void desktopControls() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(2);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *resolution = fixture.window.findChild<QComboBox *>("desktopResolutionPicker");
        auto *rotate = fixture.window.findChild<QAction *>("desktopRotateDevice");
        auto *toolbar = fixture.window.findChild<QAction *>("desktopShowToolbar");
        QVERIFY(resolution && rotate && toolbar);
        const auto original = fixture.state();
        for (int index : {0, 2, 1}) {
            resolution->setCurrentIndex(index);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            QCOMPARE(fixture.state().value("position"), original.value("position"));
            QCOMPARE(fixture.state().value("pages"), original.value("pages"));
        }
        const auto portrait = fixture.window.readerWindow()->size();
        rotate->trigger();
        QTRY_COMPARE(fixture.window.readerWindow()->size(), portrait.transposed());
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(opened.count(), 1); // Reflow reuses the open publication.
        rotate->trigger();
        QTRY_COMPARE(fixture.window.readerWindow()->size(), portrait);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("position"), original.value("position"));
        toolbar->trigger();
        QTRY_VERIFY(!fixture.window.findChild<QWidget *>("desktopPreviewToolbar")->isVisible());
        toolbar->trigger();
        QTRY_VERIFY(fixture.window.findChild<QWidget *>("desktopPreviewToolbar")->isVisible());
        const int page = fixture.state().value("page").toInt();
        auto *next = fixture.window.findChild<QPushButton *>("desktopDeviceNext");
        QVERIFY(next && next->isVisible());
        QTest::mouseClick(next, Qt::LeftButton);
        QTRY_VERIFY(!fixture.state().value("busy").toBool() && fixture.state().value("page").toInt() > page);
        auto *previous = fixture.window.findChild<QPushButton *>("desktopDevicePrevious");
        QVERIFY(previous);
        QTest::mouseClick(previous, Qt::LeftButton);
        QTRY_VERIFY(!fixture.state().value("busy").toBool() && fixture.state().value("page").toInt() == page);
    }
    void booxRetirementFrame() {
        QTemporaryDir directory;
        storage::Paths paths{directory.path()};
        QVERIFY(storage::writeJson(paths.preferences(), {{"default_menu_view", "none"}, {"highlight_help_seen", true}}));
        android::ReaderController controller(paths, engineFontInputs(*assets, {}, {}, {}),
                                             placeholderFontFamily(*assets, *copies), desktop::readerServices,
                                             [](Qt::ScreenOrientation) {});
        const auto drain = qScopeGuard([&] {
            controller.close();
            if (!QTest::qWaitFor([&] { return controller.idle(); }, 10000))
                QTest::qFail("Reader did not drain after retirement", __FILE__, __LINE__);
        });
        QWidget window;
        auto *layout = new QStackedLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *canvas = new android::WidgetCanvas(&window);
        layout->addWidget(canvas);
        window.resize(600, 800);
        window.show();
        android::SurfaceHandoff handoff(window);
        connect(&controller, &android::ReaderController::opened, &window, [&] {
            canvas->setContent(controller.window());
            canvas->fitContent();
            controller.window()->show();
        });
        QSignalSpy opened(&controller, &android::ReaderController::opened);
        QSignalSpy retiring(&controller, &android::ReaderController::retiring);
        bool visibleAtRetirement = false;
        QImage lastFrame;
        connect(&controller, &android::ReaderController::retiring, &window, [&] {
            visibleAtRetirement = controller.window() && controller.window()->isVisible();
            handoff.hold();
            lastFrame = window.grab().toImage();
        });
        controller.open(book, QSize(600, 800));
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(controller.window()->readyForPresentation());
        QCoreApplication::processEvents();
        canvas->viewport()->repaint();
        const auto before = window.grab().toImage();
        int beforeInk = 0;
        for (int y = 0; y < before.height(); ++y)
            for (int x = 0; x < before.width(); ++x)
                beforeInk += qGray(before.pixel(x, y)) < 100;
        QVERIFY(beforeInk > 100);
        handoff.capture();
        controller.window()->close();
        QTRY_VERIFY_WITH_TIMEOUT(controller.idle(), 10000);
        QCOMPARE(retiring.count(), 1);
        QVERIFY(visibleAtRetirement);
        QVERIFY(handoff.active());
        QVERIFY(!lastFrame.isNull());
        int ink = 0;
        for (int y = 0; y < lastFrame.height(); ++y)
            for (int x = 0; x < lastFrame.width(); ++x)
                ink += qGray(lastFrame.pixel(x, y)) < 100;
        QVERIFY(ink > 100);
    }
    void booxMenuDismissalRefresh_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void booxMenuDismissalRefresh() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences["default_menu_view"] = "none";
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        auto *reader = fixture.window.readerWindow();
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        QVERIFY(reader && canvas);
        const QRegion damage(QRect(0, 0, 100, 100));
        android::takeRefreshFrame(reader, canvas->viewport(), damage);
        for (int cycle = 0; cycle < 3; ++cycle) {
            QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(fixture.visible("rrControlsTop"));
            const auto opening = android::takeRefreshFrame(reader, canvas->viewport(), damage);
            QCOMPARE(opening.clean, dark);
            QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(!fixture.visible("rrControlsTop"));
            QTRY_VERIFY(reader->property("reader_full_flash").toBool());
            // An unavailable Android surface must retain the cleanup request.
            bool rejectedClean = false;
            QVERIFY(!android::prepareRefreshFrame(reader, canvas->viewport(), damage, [&](const auto &frame) {
                rejectedClean = frame.clean;
                return false;
            }));
            QVERIFY(rejectedClean);
            QVERIFY(reader->property("reader_full_flash").toBool());
            android::BooxFrameQueue queue;
            int cleanFrames = 0;
            QVERIFY(android::prepareRefreshFrame(reader, canvas->viewport(), damage, [&](const auto &frame) {
                return queue.prepare(frame, [&](const auto &submitted) {
                    cleanFrames += submitted.clean;
                    return true;
                });
            }));
            QCOMPARE(cleanFrames, 1);
            QVERIFY(!queue.pending());
            QVERIFY(!android::takeRefreshFrame(reader, canvas->viewport(), damage).clean);
        }
    }
    void bookmarkReturn_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QString>("menuView");
        QTest::addColumn<QString>("detour");
        for (bool dark : {false, true}) {
            const auto theme = dark ? "dark" : "light";
            for (const auto &mode : {"none", "stats", "seek"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode, theme))) << dark << QString(mode) << QString();
            for (const auto &tab : {"Stats", "Chapters"})
                QTest::newRow(qPrintable(QString("default-via-%1-%2").arg(tab, theme)))
                    << dark << QString("none") << QString("rrNav%1").arg(tab);
        }
    }
    void bookmarkReturn() {
        QFETCH(bool, dark);
        QFETCH(QString, menuView);
        QFETCH(QString, detour);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences["default_menu_view"] = menuView;
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
        const auto destination = fixture.state().value("position");
        for (int i = 0; i < 4; ++i) {
            const int previous = fixture.state().value("page").toInt();
            fixture.window.readerWindow()->pageButton(1);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            QTRY_VERIFY(fixture.state().value("page").toInt() > previous);
        }
        const auto origin = fixture.state().value("position");
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrBookmarks"));
        QVERIFY(fixture.press("rrBookmark0"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("position"), destination);
        QVERIFY(fixture.press("rrPage"));
        if (!detour.isEmpty()) {
            QVERIFY(fixture.press(qPrintable(detour)));
            QTRY_VERIFY(fixture.visible(detour == "rrNavStats" ? "rrChapterGraph" : "rrChoices"));
        }
        if (menuView != "seek")
            QVERIFY(fixture.press("rrNavSeek"));
        auto *scrubber = dynamic_cast<ui::PageScrubber *>(fixture.widget("rrPageScrubber"));
        QVERIFY(scrubber);
        QTRY_VERIFY(scrubber->previewReady());
        QTRY_VERIFY(fixture.visible("rrScrubberReturn"));
        QVERIFY(fixture.press("rrScrubberReturn"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("position"), origin);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(!fixture.visible("rrScrubberReturn"));
    }
    void navigation_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("defaultNavigate");
        QTest::newRow("light") << false << false;
        QTest::newRow("dark") << true << false;
        QTest::newRow("default-navigate-light") << false << true;
        QTest::newRow("default-navigate-dark") << true << true;
    }
    void navigation() {
        QFETCH(bool, dark);
        QFETCH(bool, defaultNavigate);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences["default_menu_view"] = defaultNavigate ? "seek" : "none";
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        if (!defaultNavigate)
            openPrologueBody(fixture);
        const auto origin = fixture.state().value("position");
        if (!fixture.visible("rrBottomNavigation"))
            QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrBottomNavigation"));
        if (!defaultNavigate) {
            auto *bar = fixture.widget("rrBottomNavigation");
            QCOMPARE(bar->grab().toImage().pixelColor(bar->width() / 2, 0), bar->palette().color(QPalette::WindowText));
            QVERIFY(fixture.press("rrNavSeek"));
        }
        QTRY_VERIFY(fixture.visible("rrBookPosition"));
        auto *scrubber = dynamic_cast<ui::PageScrubber *>(fixture.widget("rrPageScrubber"));
        QVERIFY(scrubber);
        QTRY_VERIFY(scrubber->previewReady());
        if (defaultNavigate) {
            auto *bar = fixture.widget("rrBottomNavigation");
            QCOMPARE(bar->grab().toImage().pixelColor(bar->width() / 2, 0), bar->palette().color(QPalette::WindowText));
            auto *seek = fixture.widget("rrNavSeek");
            QVERIFY(seek && seek->isHidden());
            const int x = seek->mapTo(bar, seek->rect().center()).x();
            QCOMPARE(bar->grab().toImage().pixelColor(x, int(ui::metrics::control::border)),
                     bar->palette().color(QPalette::Window));
        }
        const auto output = qEnvironmentVariable("KOBALT_TEST_CAPTURE_DIR");
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(fixture.window.grab().save(output + (dark ? "/navigate-dark.png" : "/navigate-light.png")));
        }
        const int initial = scrubber->selectedPage();
        if (defaultNavigate) {
            QCOMPARE(initial, 0);
            fixture.window.readerWindow()->pageButton(-1);
            QCOMPARE(scrubber->selectedPage(), 0);
            QCOMPARE(fixture.state().value("position"), origin);
        }
        fixture.window.readerWindow()->pageButton(1);
        QTRY_COMPARE(scrubber->selectedPage(), initial + 1);
        QCOMPARE(fixture.state().value("position"), origin);
        fixture.window.readerWindow()->pageButton(-1);
        QTRY_COMPARE(scrubber->selectedPage(), initial);
        QCOMPARE(fixture.state().value("position"), origin);
        if (defaultNavigate) {
            fixture.window.readerWindow()->pageButton(1);
            QTRY_COMPARE(scrubber->selectedPage(), initial + 1);
            QTRY_VERIFY(scrubber->previewReady());
            QVERIFY(fixture.click(scrubber, scrubber->previewBounds().center()));
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            QTRY_COMPARE(fixture.state().value("page").toInt(), initial + 1);
            return;
        }
        QVERIFY(fixture.click(scrubber->slider(), {scrubber->slider()->width() * 3 / 4, scrubber->slider()->height() / 2}));
        QTRY_VERIFY(scrubber->previewReady());
        const int destination = scrubber->selectedPage();
        QVERIFY(destination != fixture.state().value("page").toInt());
        QCOMPARE(fixture.state().value("position"), origin);
        const auto center = scrubber->previewBounds().center();
        const int distance = scrubber->previewBounds().width() / 3;
        QVERIFY(fixture.drag(scrubber, center + QPoint(distance / 2, 0), center - QPoint(distance / 2, 0)));
        QTRY_COMPARE(scrubber->selectedPage(), destination + 1);
        QTRY_VERIFY(scrubber->previewReady());
        QCOMPARE(fixture.state().value("position"), origin);
        QVERIFY(fixture.drag(scrubber, center - QPoint(distance / 2, 0), center + QPoint(distance / 2, 0)));
        QTRY_COMPARE(scrubber->selectedPage(), destination);
        QTRY_VERIFY(scrubber->previewReady());
        // A vertical drag must neither open the preview nor turn a page.
        QVERIFY(fixture.drag(scrubber, center, center + QPoint(0, distance)));
        QCOMPARE(scrubber->selectedPage(), destination);
        QCOMPARE(fixture.state().value("position"), origin);
        QVERIFY(fixture.click(fixture.widget("rrPreviewNextPage"), {10, fixture.widget("rrPreviewNextPage")->height() / 2}));
        QTRY_COMPARE(scrubber->selectedPage(), destination + 1);
        QTRY_VERIFY(scrubber->previewReady());
        QVERIFY(fixture.click(fixture.widget("rrPreviewPreviousPage"), {fixture.widget("rrPreviewPreviousPage")->width() - 10, fixture.widget("rrPreviewPreviousPage")->height() / 2}));
        QTRY_COMPARE(scrubber->selectedPage(), destination);
        QTRY_VERIFY(scrubber->previewReady());
        QVERIFY(fixture.press("rrNavSeek"));
        QTRY_VERIFY(!scrubber->selecting());
        QCOMPARE(fixture.state().value("position"), origin);
        QVERIFY(fixture.press("rrNavSeek"));
        QTRY_VERIFY(fixture.visible("rrBookPosition"));
        QVERIFY(fixture.click(scrubber->slider(), {scrubber->slider()->width() * 3 / 4, scrubber->slider()->height() / 2}));
        QTRY_VERIFY(scrubber->previewReady());
        const int accepted = scrubber->selectedPage();
        QVERIFY(fixture.click(scrubber, scrubber->previewBounds().center()));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("page").toInt(), accepted);
        QCOMPARE(opened.count(), 1);

        // Exercise real touch sequences independently of mouse emulation.
        ui::PageScrubber touchPreview(nullptr);
        touchPreview.resize(800, 1200);
        touchPreview.setBook(1, 3, {}, {600, 900});
        touchPreview.show();
        QVERIFY(QTest::qWaitForWindowExposed(&touchPreview));
        touchPreview.previewRequested = [&](int page, QSize size) {
            QImage image(size, QImage::Format_RGB32);
            image.fill(Qt::white);
            touchPreview.setPreview(page, image);
        };
        int committed = -1;
        touchPreview.accepted = [&](int page) { committed = page; };
        touchPreview.showCurrentPage();
        QTRY_VERIFY(touchPreview.previewReady());
        auto *device = QTest::createTouchDevice();
        auto touch = QTest::touchEvent(&touchPreview, device, false);
        const auto middle = touchPreview.previewBounds().center();
        const QPoint travel(100, 0);
        const auto swipe = [&](QPoint from, QPoint to) {
            touch.press(0, from).commit();
            touch.move(0, to).commit();
            touch.release(0, to).commit();
        };
        swipe(middle + travel, middle - travel);
        QCOMPARE(touchPreview.selectedPage(), 2);
        QTRY_VERIFY(touchPreview.previewReady());
        QCOMPARE(committed, -1);
        swipe(middle + travel, middle - travel);
        QCOMPARE(touchPreview.selectedPage(), 2); // Last page clamps.
        swipe(middle - travel, middle + travel);
        QCOMPARE(touchPreview.selectedPage(), 1);
        QTRY_VERIFY(touchPreview.previewReady());
        swipe(middle - travel, middle + travel);
        QCOMPARE(touchPreview.selectedPage(), 0);
        QTRY_VERIFY(touchPreview.previewReady());
        swipe(middle - travel, middle + travel);
        QCOMPARE(touchPreview.selectedPage(), 0); // First page clamps.
        auto *side = touchPreview.findChild<QWidget *>("rrPreviewNextPage");
        QVERIFY(side && side->isVisible());
        const auto sideStart = side->pos() + QPoint(10, side->height() / 2);
        swipe(sideStart, sideStart - travel);
        QCOMPARE(touchPreview.selectedPage(), 1);
        QTRY_VERIFY(touchPreview.previewReady());
        QCOMPARE(committed, -1);
        // Multiple fingers cancel the whole sequence, even after one lifts.
        touch.press(0, middle).commit();
        touch.stationary(0).press(1, middle + travel).commit();
        touch.move(0, middle - travel).stationary(1).commit();
        touch.release(0, middle - travel).release(1, middle + travel).commit();
        QCOMPARE(touchPreview.selectedPage(), 1);
        QCOMPARE(committed, -1);
        touch.press(0, middle).commit();
        QEvent cancel(QEvent::TouchCancel);
        QApplication::sendEvent(&touchPreview, &cancel);
        touch.release(0, middle).commit();
        QCOMPARE(committed, -1);
        touch.press(0, middle).commit();
        touch.release(0, middle).commit();
        QCOMPARE(committed, 1);
    }
    void annotationTwoFingerTapConsumesWholeSequence() {
        ReadingPage page(nullptr, [] {});
        page.resize(600, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto *device = QTest::createTouchDevice();
        auto touch = QTest::touchEvent(&page, device, false);
        const QPoint first(240, 420), second(340, 470);
        int requests = 0, taps = 0, holds = 0, swipes = 0, lightChanges = 0;
        page.annotationRequested = [&] {
            ++requests;
        };
        page.tapped = [&](QPoint) {
            ++taps;
        };
        page.held = [&](QPoint) {
            ++holds;
        };
        page.swiped = [&](int) {
            ++swipes;
        };
        page.lightChanged = [&](double) {
            ++lightChanges;
        };
        auto releaseBoth = [&] {
            touch.release(0, first, &page).release(1, second, &page).commit();
            QCoreApplication::processEvents();
        };
        touch.press(0, first, &page).press(1, second, &page).commit();
        releaseBoth();
        QCOMPARE(requests, 1);
        QCOMPARE(taps + holds + swipes + lightChanges, 0);
        // Releasing one finger must not turn the remaining finger into a tap.
        touch.press(0, first, &page).commit();
        touch.move(0, first + QPoint(1, 0), &page).commit();
        touch.stationary(0).press(1, second, &page).commit();
        touch.release(1, second, &page).stationary(0).commit();
        touch.release(0, first, &page).commit();
        QCOMPARE(requests, 2);
        QCOMPARE(taps + holds + swipes + lightChanges, 0);
        // A failed two-finger swipe is consumed, including its last release.
        touch.press(0, first, &page).press(1, second, &page).commit();
        touch.move(0, first + QPoint(180, 0), &page).stationary(1).commit();
        releaseBoth();
        QCOMPARE(requests, 2);
        QCOMPARE(taps + holds + swipes + lightChanges, 0);
        // Three contacts and late second contacts never become shortcuts.
        touch.press(0, first, &page).press(1, second, &page).press(2, QPoint(300, 430), &page).commit();
        touch.release(0, first, &page).release(1, second, &page).release(2, QPoint(300, 430), &page).commit();
        touch.press(0, first, &page).commit();
        QTest::qWait(170);
        touch.stationary(0).press(1, second, &page).commit();
        releaseBoth();
        QCOMPARE(requests, 2);
        QCOMPARE(taps + holds + swipes + lightChanges, 0);
        touch.press(0, first, &page).press(1, second, &page).commit();
        QTest::qWait(370);
        releaseBoth();
        QCOMPARE(requests, 2);
        QCOMPARE(taps + holds + swipes + lightChanges, 0);
        touch.press(0, QPoint(60, 420), &page).press(1, second, &page).commit();
        touch.release(0, QPoint(60, 420), &page).release(1, second, &page).commit();
        touch.press(0, first, &page).press(1, second, &page).commit();
        QTouchEvent cancelled(QEvent::TouchCancel, device);
        QApplication::sendEvent(&page, &cancelled);
        releaseBoth();
        QCOMPARE(requests, 2);
        QCOMPARE(taps + holds + swipes + lightChanges, 0);
        // Single-finger reading remains available after failed gestures.
        touch.press(0, first, &page).commit();
        touch.release(0, first, &page).commit();
        QCOMPARE(taps, 1);
        QCOMPARE(holds + swipes + lightChanges, 0);
    }
    void annotationRightClickRejectsChords() {
        ReadingPage page(nullptr, [] {});
        page.resize(600, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        int requests = 0;
        page.annotationRequested = [&] {
            ++requests;
        };
        const QPoint point(300, 450);
        QTest::mouseRelease(&page, Qt::RightButton, Qt::NoModifier, point);
        QCOMPARE(requests, 0);
        QTest::mousePress(&page, Qt::RightButton, Qt::NoModifier, point);
        QTest::mousePress(&page, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(&page, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(&page, Qt::RightButton, Qt::NoModifier, point);
        QCOMPARE(requests, 0);
        QTest::mousePress(&page, Qt::RightButton, Qt::NoModifier, point);
        QTest::mouseRelease(&page, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(&page, Qt::RightButton, Qt::NoModifier, point);
        QCOMPARE(requests, 0);
        QTest::mouseClick(&page, Qt::RightButton, Qt::NoModifier, point);
        QCOMPARE(requests, 1);
    }
    void stylesheet() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        // Isolate CSS reload from the publisher styles of the general book.
        QVERIFY(createBook(fixture.directory.path()));
        const auto styledBook = fixture.directory.path() + "/text.epub";
        const auto cssPath = fixture.paths.directory + "/userstyle.css";
        QFile css(cssPath);
        const QByteArray rules = "p { color: #ff0000 !important; }";
        QVERIFY(css.open(QIODevice::WriteOnly));
        QCOMPARE(css.write(rules), rules.size());
        css.close();
        const auto redPixels = [&] {
            const auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
            if (!page)
                return 0;
            const auto image = page->image.convertToFormat(QImage::Format_RGB32);
            int red = 0;
            for (int y = 0; y < image.height(); ++y) {
                const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
                for (int x = 0; x < image.width(); ++x)
                    red += qRed(row[x]) > 180 && qGreen(row[x]) < 80 && qBlue(row[x]) < 80;
            }
            return red;
        };
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(styledBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        QVERIFY(redPixels() > 100);
        QVERIFY(QFile::remove(cssPath));
        QVERIFY(redPixels() > 100); // Changes apply on the next opening.
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrReturn"));
        QVERIFY(fixture.press("rrReturn"));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
        fixture.window.openBook(styledBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        QCOMPARE(redPixels(), 0);
    }
};

class ScreenshotTests final : public QObject {
    Q_OBJECT
private slots:
    void directLinks_data() { variants(); }
    void directLinks() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = QStringList{"fill", "six-inch", "seven-inch"}.at(preset) + (dark ? "-dark" : "-light");
        QVERIFY(writeBook(fixture.directory.path(), "links.epub", test::linkBook()));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(fixture.directory.path() + "/links.epub");
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(capture(fixture, name + "-tappable-links-off"));
        for (const auto *mode : {"footnotes", "all"}) {
            QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(fixture.visible("rrSettings"));
            QVERIFY(fixture.press("rrSettings"));
            QVERIFY(fixture.press("rrSettingsControls"));
            if (QString(mode) == "footnotes")
                QVERIFY(capture(fixture, name + "-tappable-links-settings"));
            QVERIFY(fixture.press("rrTappableLinks"));
            QTRY_VERIFY(fixture.visible("rrTappableLinks_off"));
            if (QString(mode) == "footnotes")
                QVERIFY(capture(fixture, name + "-tappable-links-choices"));
            QVERIFY(fixture.press(qPrintable(QString("rrTappableLinks_") + mode)));
            QVERIFY(fixture.press("rrSettingsClose"));
            QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
            if (fixture.visible("rrControlsTop"))
                QVERIFY(fixture.press("rrPage"));
            QVERIFY(capture(fixture, name + "-tappable-links-" + mode));
        }
    }
    void screens_data() { variants(); }
    void screens() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark, false, false, true, true);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto device = QStringList{"fill", "six-inch", "seven-inch"}.at(preset);
        const auto name = device + (dark ? "-dark" : "-light");
        // Onboarding is part of the Library and follows both themes.
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QTRY_VERIFY(start && start->width() > 300);
        QVERIFY(capture(fixture, name + "-library-empty"));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        auto *reader = fixture.window.readerWindow();
        QVERIFY(reader);
        QCOMPARE(reader->property("reader_dark").toBool(), dark);
        QVERIFY(hasText(reader->findChild<QWidget *>("rrPage")->grab().toImage()));
        QVERIFY(capture(fixture, name + "-cover"));
        openPrologueBody(fixture);
        QVERIFY(capture(fixture, name + "-reading"));
        tour(fixture, name, true);
    }
    void library_data() { variants(); }
    void library() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        const auto device = QStringList{"fill", "six-inch", "seven-inch"}.at(preset);
        const auto name = device + (dark ? "-dark" : "-light") + "-library";
        {
            Fixture fixture(dark);
            QVERIFY(fixture.prepared);
            fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
            auto *start = fixture.window.findChild<android::StartScreen *>();
            auto *view = start->library();
            auto *canvas = start->libraryCanvas();
            QTRY_VERIFY(view->isVisible() && view->size() == canvas->sceneRect().size().toSize() &&
                        view->width() > 600);
            view->findChild<QPushButton *>("libraryMore")->click();
            view->findChild<QPushButton *>("libraryMoreFolders")->click();
            QTRY_VERIFY(view->findChild<QWidget *>("libraryFolders")->isVisible());
            QVERIFY(capture(fixture, name + "-folders-empty"));
        }
        for (const bool reading : {true, false}) {
            Fixture fixture(dark, false, true, reading);
            QVERIFY(fixture.prepared);
            fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
            auto *start = fixture.window.findChild<android::StartScreen *>();
            QVERIFY(start);
            start->showLibrary();
            auto *view = start->library();
            // The preset applies on a short timer; wait until the library has the reader's size.
            auto *canvas = start->libraryCanvas();
            QTRY_VERIFY(view->isVisible() && view->size() == canvas->sceneRect().size().toSize() &&
                        view->width() > 600);
            // The library opens as a cover grid.
            QVERIFY(capture(fixture, name + (reading ? "-grid" : "-grid-new")));
            const auto press = [view](const char *control) {
                auto *button = view->findChild<QAbstractButton *>(control);
                QVERIFY2(button && button->isVisible() && button->isEnabled(), control);
                button->click();
            };
            const auto bookMenu = [view] {
                auto *pages = view->findChild<QWidget *>("libraryPages");
                const QPoint point(pages->width() / 8, pages->height() / 8);
                QContextMenuEvent menu(QContextMenuEvent::Mouse, point, pages->mapToGlobal(point));
                QApplication::sendEvent(pages, &menu);
                QVERIFY(view->findChild<QWidget *>("libraryBookMenu"));
            };
            if (reading) {
                // A pressed cover gets its frame. Releasing on paper opens nothing.
                auto *pages = view->findChild<QWidget *>("libraryPages");
                QVERIFY(pages);
                const QPoint book(pages->width() / 8, pages->height() / 8);
                QTest::mousePress(pages, Qt::LeftButton, Qt::NoModifier, book);
                QVERIFY(capture(fixture, name + "-pressed"));
                QTest::mouseRelease(pages, Qt::LeftButton, Qt::NoModifier, QPoint(pages->width() - 2, pages->height() - 2));
                // The book's menu, opened at once with a secondary click rather than a timed hold.
                QContextMenuEvent menu(QContextMenuEvent::Mouse, book, pages->mapToGlobal(book));
                QApplication::sendEvent(pages, &menu);
                QVERIFY(view->findChild<QWidget *>("libraryBookMenu"));
                QVERIFY(capture(fixture, name + "-book-menu"));
                press("libraryBookMenuRemove");
                QTRY_VERIFY(view->findChild<QPushButton *>("libraryRemoveCancel")->isVisible());
                QVERIFY(capture(fixture, name + "-remove-confirmation"));
                press("libraryRemoveCancel");
            } else {
                bookMenu();
                QVERIFY(capture(fixture, name + "-new-book-menu"));
                QVERIFY(view->dismiss());
            }
            // Home shows only the books in progress, or a note when there are none.
            press("libraryViewHome");
            QVERIFY(capture(fixture, name + (reading ? "-home" : "-home-empty")));
            if (!reading)
                continue;
            bookMenu();
            QVERIFY(capture(fixture, name + "-reading-book-menu"));
            QVERIFY(view->dismiss());
            press("libraryViewList");
            QVERIFY(capture(fixture, name + "-list"));
            press("libraryNext");
            QVERIFY(capture(fixture, name + "-list-next"));
            press("libraryPrevious");
            press("librarySearch");
            auto *query = view->findChild<QLineEdit *>("librarySearchQuery");
            QVERIFY(query && query->isVisible());
            query->setText("Neralis");
            QVERIFY(capture(fixture, name + "-search-results"));
            query->setText("No matching invented book");
            QVERIFY(capture(fixture, name + "-search-empty"));
            press("libraryCloseSearch");
            press("librarySort");
            QVERIFY(capture(fixture, name + "-sort"));
            auto *bySeries = view->findChild<QAbstractButton *>("librarySortSeries");
            QVERIFY(bySeries);
            bySeries->click();
            QVERIFY(capture(fixture, name + "-list-series"));
            press("libraryViewGrid");
            press("librarySort");
            QVERIFY(capture(fixture, name + "-grid-sort"));
            press("libraryCoverSmall");
            QVERIFY(capture(fixture, name + "-grid-small"));
            press("librarySort");
            press("libraryCoverLarge");
            QVERIFY(capture(fixture, name + "-grid-large"));
            press("libraryPrevious"); // The pager wraps from the first page to the last.
            QVERIFY(capture(fixture, name + "-grid-last"));
            press("libraryMore");
            QVERIFY(capture(fixture, name + "-more"));
            press("libraryMoreFolders");
            QTRY_VERIFY(view->findChild<QWidget *>("libraryFolders")->isVisible());
            QVERIFY(capture(fixture, name + "-folders"));
            press("libraryFoldersBack");
            view->setScanning(true, 17);
            auto *progress = dynamic_cast<ui::LoadingIndicator *>(view->findChild<QWidget *>("libraryScanProgress"));
            QVERIFY(progress);
            QTRY_VERIFY(progress->isVisible() && progress->isRunning());
            // Capture the same revealed animation frame on every run.
            auto *timer = progress->findChild<QTimer *>();
            QVERIFY(timer);
            progress->start(0);
            timer->stop();
            QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
            timer->stop();
            QVERIFY(capture(fixture, name + "-scanning"));
            view->setScanning(false, 17);
            start->showLibraryError("This EPUB could not be opened. Choose another book or try again.");
            QTRY_VERIFY(view->findChild<QPushButton *>("libraryNoticeOk")->isVisible());
            QVERIFY(capture(fixture, name + "-opening-error"));
        }
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "minimal:enable_fonts:freetype");
    FontCopies fontCopies;
    copies = &fontCopies;
    QApplication app(argc, argv);
    if (QGuiApplication::platformName() != "minimal") {
        qCritical() << "This executable only runs headlessly.";
        return 2;
    }
    app.setApplicationName("Kobalt headless tests");
    app.setStyle("Fusion");
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir fixtureDirectory(QDir::tempPath() + "/kobalt-book-XXXXXX");
    AssetBundle bundle(QStringLiteral(KOBALT_TEST_ASSETS), QStringLiteral(KOBALT_VERSION));
    assets = &bundle;
    if (!fixtureDirectory.isValid() || !bundle.available() ||
        !ui::initializeInterfaceFonts(bundle)) {
        qCritical() << "Cannot prepare the test book or runtime assets.";
        return 2;
    }
    for (const auto &path : bundledFontFiles(bundle)) {
        if (QFontDatabase::addApplicationFont(path) < 0) {
            qCritical() << "Cannot register bundled font:" << path;
            return 2;
        }
    }
    readingPpi.store(300);
    ui::setDensity(300);
    app.setFont(ui::interfaceFont());
    auto arguments = app.arguments();
    if (arguments.size() == 3 && arguments.at(1) == "--regression") {
        // Share the offline EPUB fixture with the restored standalone checks.
        const auto name = QFileInfo(arguments.at(2)).fileName();
        const bool lookup = name == "kobalt-search-tests" || name == "kobalt-word-selection-tests";
        const bool links = name == "kobalt-links-tests";
        QMap<QString, test::BookFiles> engineBooks;
        QStringList inputs{fixtureDirectory.path() + "/text.epub"};
        if (name == "kobalt-document-cache-tests") {
            engineBooks = {{"text.epub", test::engineBook()}};
        } else if (name == "kobalt-reader-tests") {
            engineBooks = {{"text.epub", test::engineBook()}, {"color.epub", test::engineBook(true)}};
            inputs.append(fixtureDirectory.path() + "/color.epub");
        } else if (name == "kobalt-local-reader-tests") {
            engineBooks = {{"text.epub", test::engineBook(false, 2, 300)}, {"color.epub", test::engineBook(true)}};
            inputs.append(fixtureDirectory.path() + "/color.epub");
        } else if (name == "kobalt-document-word-count-tests") {
            engineBooks = {{"text.epub", test::wordCountBook()}, {"word-count-many.epub", test::wordCountBook(1500)}};
            inputs.append(fixtureDirectory.path() + "/word-count-many.epub");
        } else if (name == "kobalt-publisher-markup-tests") {
            QFile font(bundle.file("fonts/IBMPlexSans-Regular.ttf"));
            if (!font.open(QIODevice::ReadOnly))
                return 2;
            engineBooks = test::publisherBooks(font.readAll());
            inputs = {fixtureDirectory.path() + "/publisher.epub", fixtureDirectory.path() + "/publisher-de.epub",
                      fixtureDirectory.path() + "/publisher-ru.epub"};
        } else if (!(lookup || links ? writeBook(fixtureDirectory.path(), "text.epub", links ? test::linkBook() : test::lookupBook())
                                     : createBook(fixtureDirectory.path()))) {
            return 2;
        }
        for (auto it = engineBooks.cbegin(); it != engineBooks.cend(); ++it)
            if (it.value().isEmpty() || !writeBook(fixtureDirectory.path(), it.key(), it.value()))
                return 2;
        QProcess child;
        child.setProcessChannelMode(QProcess::ForwardedChannels);
        if (QFileInfo(arguments.at(2)).fileName() == "kobalt-reader-session-tests") {
            for (const auto *name : {"formatting-unsupported-writing.epub", "later-images.epub", "publisher-non-linear.epub"})
                if (!createBook(fixtureDirectory.path(), name))
                    return 2;
            inputs.append(fixtureDirectory.path() + "/publisher-non-linear.epub");
        }
        child.start(arguments.at(2), inputs);
        if (!child.waitForFinished(20000)) {
            const auto error = child.errorString();
            child.kill();
            child.waitForFinished();
            qCritical() << "Regression process did not finish:" << error;
            return 2;
        }
        return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : 2;
    }
    const bool screenshots = arguments.size() >= 2 && arguments.at(1) == "--screenshots";
    QString output;
    std::unique_ptr<QTemporaryDir> staging;
    if (screenshots) {
        if (arguments.size() != 3 || !QDir::isAbsolutePath(arguments.at(2))) {
            qCritical() << "Usage: kobalt-desktop-tests --screenshots ABSOLUTE_DIRECTORY";
            return 2;
        }
        output = QDir::cleanPath(arguments.at(2));
        if (QFileInfo::exists(output)) {
            QFile previous(output + "/manifest.json");
            if (!previous.open(QIODevice::ReadOnly) ||
                QJsonDocument::fromJson(previous.readAll()).object().value("generator") != "kobalt-desktop") {
                qCritical() << "Refusing to replace a directory not created by this screenshot generator:" << output;
                return 2;
            }
        }
        const auto parent = QFileInfo(output).absolutePath();
        if (!QDir().mkpath(parent)) {
            return 2;
        }
        // The CMake parent owns TMPDIR and can clean captures after a timeout.
        staging = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/kobalt-captures-XXXXXX");
        captureDirectory = staging->path() + "/images";
        if (!staging->isValid() || !QDir().mkpath(captureDirectory)) {
            return 2;
        }
        arguments = {arguments.first()};
    }
    DesktopTests tests;
    ScreenshotTests screens;
    QTimer::singleShot(0, &app, [&] {
        app.exit(QTest::qExec(screenshots ? static_cast<QObject *>(&screens) : &tests, arguments));
    });
    const int result = app.exec();
    if (result != 0 || !screenshots) {
        return result;
    }
    QFile fixture(book);
    constexpr int expectedScreenshots = 344;
    if (captures.size() != expectedScreenshots)
        qCritical() << "Expected" << expectedScreenshots << "screenshots, captured" << captures.size();
    if (!fixture.open(QIODevice::ReadOnly) || captures.size() != expectedScreenshots) {
        return 2;
    }
    const QJsonObject manifest{{"schema", 1}, {"generator", "kobalt-desktop"}, {"qt", qVersion()}, {"version", KOBALT_VERSION},
                               {"commit", kobaltCommit}, {"modified", bool(KOBALT_COMMIT_MODIFIED)},
                               {"book", "Trevelyan's Type Tester v1.2 with chapter-heading CSS fix"}, {"book_file", QFileInfo(book).fileName()},
                               {"book_sha256", QString::fromLatin1(QCryptographicHash::hash(fixture.readAll(), QCryptographicHash::Sha256).toHex())},
                               {"images", captures}};
    QSaveFile record(captureDirectory + "/manifest.json");
    const auto json = QJsonDocument(manifest).toJson();
    if (!record.open(QIODevice::WriteOnly) || record.write(json) != json.size() || !record.commit()) {
        return 2;
    }
    QString gallery = R"HTML(<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Kobalt screenshots</title>
<style>body{font:16px system-ui;margin:24px;background:#eee;color:#222}header{position:sticky;top:0;background:#eee;padding:12px 0}input{font:inherit;padding:8px;width:320px;max-width:90%}main{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:20px}figure{margin:0;background:white;padding:12px;border:1px solid #ccc}figure[hidden]{display:none}img{width:100%;height:380px;object-fit:contain;background:#ddd}figcaption{padding-top:8px;overflow-wrap:anywhere}a{color:inherit}</style>
<header><h1>Kobalt screenshots</h1><p>Actual native app views. Open an image for full size. <a href="manifest.json">Checksums and build details</a></p>
<input aria-label="Filter screenshots" placeholder="Filter: six-inch, dark, search…" oninput="for(const f of document.querySelectorAll('figure')) f.hidden=!f.textContent.toLowerCase().includes(this.value.toLowerCase())"></header><main>
)HTML";
    for (const auto &item : captures) {
        const auto entry = item.toObject();
        const auto file = entry.value("file").toString().toHtmlEscaped();
        gallery += QString("<figure><a href=\"%1\"><img loading=\"lazy\" src=\"%1\" alt=\"%1\"></a>"
                           "<figcaption>%1 · %2 × %3</figcaption></figure>\n")
                       .arg(file).arg(entry.value("width").toInt()).arg(entry.value("height").toInt());
    }
    gallery += "</main></html>\n";
    QSaveFile index(captureDirectory + "/index.html");
    const auto html = gallery.toUtf8();
    if (!index.open(QIODevice::WriteOnly) || index.write(html) != html.size() || !index.commit()) {
        return 2;
    }
    // Keep the last complete set outside TMPDIR until replacement succeeds.
    const auto previous = QFileInfo(output).absolutePath() + '/' + QFileInfo(staging->path()).fileName() + "-previous";
    const bool existing = QFileInfo::exists(output);
    if (existing && !QDir().rename(output, previous)) {
        return 2;
    }
    if (!QDir().rename(captureDirectory, output)) {
        if (existing && !QDir().rename(previous, output)) {
            qCritical() << "Previous screenshots retained at" << previous;
        }
        return 2;
    }
    if (existing && !QDir(previous).removeRecursively())
        qWarning() << "Cannot remove superseded screenshots:" << previous;
    qInfo().noquote() << "Saved" << captures.size() << "screenshots and manifest.json to" << output;
    return 0;
}

#include "desktop.moc"
