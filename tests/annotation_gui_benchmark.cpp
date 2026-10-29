#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/scale.h"
#include "interfaces/reader/views/reading_page.h"
#include "platforms/desktop/window.h"
#include "reading/reading_density.h"
#include "reading/engine_worker.h"
#include "storage/highlight_store.h"
#include "storage/bookmark_store.h"
#include "storage/reader_storage.h"
#include "support/strings.h"
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#ifdef __APPLE__
#include <mach/mach.h>
#else
#include <unistd.h>
#endif
#include <sys/resource.h>

using namespace kobalt;
namespace {
void require(bool ok, const QString &message) {
    if (!ok) throw std::runtime_error(message.toStdString());
}
QByteArray read(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read " + path);
    return file.readAll();
}
void waitFor(const std::function<bool()> &ready, const QString &message, int timeout = 120000) {
    QElapsedTimer timer;
    timer.start();
    while (!ready()) {
        require(timer.elapsed() < timeout, "Timed out: " + message);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
}
qint64 residentBytes() {
#ifdef __APPLE__
    mach_task_basic_info info{};
    mach_msg_type_number_t size = MACH_TASK_BASIC_INFO_COUNT;
    require(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &size) == KERN_SUCCESS,
            "Cannot measure resident memory");
    return info.resident_size;
#else
    QFile status("/proc/self/statm");
    require(status.open(QIODevice::ReadOnly), "Cannot measure resident memory");
    const auto fields = status.readAll().simplified().split(' ');
    require(fields.size() >= 2, "Invalid resident memory measurement");
    return fields[1].toLongLong() * sysconf(_SC_PAGESIZE);
#endif
}
double cpuMs() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
           (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
}
QJsonObject distribution(QVector<double> values) {
    require(!values.isEmpty(), "Empty timing sample");
    std::sort(values.begin(), values.end());
    QJsonArray samples;
    for (double value : values) samples.append(value);
    return {{"median_ms", values[values.size()/2]}, {"p95_ms", values[qMin(values.size()-1, (values.size()*95)/100)]},
            {"max_ms", values.last()}, {"samples_ms", samples}};
}
QVector<Highlight> fixtureHighlights(const QJsonObject &fixture, int count) {
    const auto available = fixture["highlights"].toArray();
    require(count >= 0 && count <= 2000 && available.size() == 2000, "Invalid fixture population");
    QVector<Highlight> marks;
    for (int i = 0; i < count; ++i) {
        const auto entry = available[i * available.size() / count].toObject();
        Highlight h{entry["id"].toString(), entry["start"].toString(), entry["end"].toString(), entry["text"].toString(),
                    entry["chapter"].toString(), QDateTime::fromString(entry["created"].toString(), Qt::ISODate)};
        h.color = entry["color"].toString();
        h.style = entry["style"].toString();
        h.note = entry["note"].toString();
        marks.append(h);
    }
    return marks;
}
void seedMemoryStore(const QStringList &args) {
    require(args.size() == 5 || (args.size() == 6 && args[5] == "--sqlite"),
            "Expected --store-memory-seed FIXTURE COUNT DEST [--sqlite]");
    const bool sqlite = args.size() == 6;
    qputenv("KOBALT_ANNOTATIONS_SQLITE", sqlite ? "1" : "0");
    bool validCount = false;
    const int count = args[3].toInt(&validCount);
    require(validCount, "Invalid fixture count");
    const auto fixtureBytes = read(args[2]);
    const auto fixture = QJsonDocument::fromJson(fixtureBytes).object();
    const auto marks = fixtureHighlights(fixture, count);
    // Restrict persistent fixtures to this checkout's existing build tree.
    // Resolve existing ancestors as well so symlinks cannot escape that tree.
    QDir source(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath());
    require(source.cdUp(), "Cannot locate source root");
    const QString build = QFileInfo(source.filePath("build")).canonicalFilePath();
    const QString destination = QDir::cleanPath(args[4]);
    const QFileInfo target(destination);
    require(!build.isEmpty() && QDir::isAbsolutePath(args[4]) && destination.startsWith(build + '/') &&
            !target.exists() && !target.isSymLink(), "Seed destination must be a new directory inside this checkout's build tree");
    QFileInfo ancestor(target.absolutePath());
    while (!ancestor.exists() && !ancestor.isSymLink())
        ancestor.setFile(ancestor.absolutePath());
    const auto resolvedAncestor = ancestor.canonicalFilePath();
    require(ancestor.isDir() && (resolvedAncestor == build || resolvedAncestor.startsWith(build + '/')),
            "Seed destination escapes the build tree");
    require(QDir().mkpath(destination), "Cannot create memory fixture directory");
    try {
        const QString identity = QStringLiteral("file:///synthetic/annotation-memory.epub");
        const auto fingerprint = fixture["fingerprint"].toString().toLatin1();
        HighlightStore store(destination + "/annotations", identity, fingerprint, "War and Peace",
                             AnnotationDatabase::path(destination));
        store.setHighlights(marks);
        QString error;
        require(store.save(&error), "Cannot seed memory fixture: " + error);
        qint64 textUnits = 0, chapterUnits = 0, noteUnits = 0;
        for (const auto &h : marks) {
            textUnits += h.text.size();
            chapterUnits += h.chapter.size();
            noteUnits += h.note.size();
        }
        require(storage::writeJson(destination + "/memory-fixture.json",
            {{"version", 1}, {"identity", identity}, {"fingerprint", QString::fromLatin1(fingerprint)},
             {"count", count}, {"backend", sqlite ? "sqlite" : "json"},
             {"fixture_sha256", QString::fromLatin1(QCryptographicHash::hash(fixtureBytes, QCryptographicHash::Sha256).toHex())},
             {"text_utf16_units", textUnits}, {"chapter_utf16_units", chapterUnits}, {"note_utf16_units", noteUnits}}),
            "Cannot write memory fixture manifest");
    } catch (...) {
        QDir(destination).removeRecursively();
        throw;
    }
    fprintf(stdout, "Seeded memory fixture: %s\n", qPrintable(destination));
}
void measureMemoryStore(const QStringList &args) {
    require(args.size() == 5 && (args[3] == "full" || args[3] == "compact"),
            "Expected --store-memory ROOT full|compact OUTPUT");
    const QString root = QFileInfo(args[2]).absoluteFilePath();
    const auto manifest = QJsonDocument::fromJson(read(root + "/memory-fixture.json")).object();
    require(manifest["version"].toInt() == 1 &&
            (manifest["backend"] == "sqlite" || manifest["backend"] == "json"), "Invalid memory fixture manifest");
    const bool sqlite = manifest["backend"] == "sqlite", compact = args[3] == "compact";
    qputenv("KOBALT_ANNOTATIONS_SQLITE", sqlite ? "1" : "0");
    const qint64 before = residentBytes();
    const double beginCpu = cpuMs();
    QElapsedTimer timer;
    timer.start();
    HighlightStore store(root + "/annotations", manifest["identity"].toString(),
                         manifest["fingerprint"].toString().toLatin1(), "War and Peace", AnnotationDatabase::path(root));
    require(compact ? store.loadCompact() : store.load(), "Memory probe load failed");
    const double wall = timer.nsecsElapsed()/1e6, cpu = cpuMs() - beginCpu;
    const qint64 after = residentBytes();
    require(store.highlights().size() == manifest["count"].toInt(), "Memory probe lost records");
    qint64 textUnits = 0, chapterUnits = 0, noteUnits = 0;
    int loadedDetails = 0;
    for (const auto &h : store.highlights()) {
        textUnits += h.text.size();
        chapterUnits += h.chapter.size();
        noteUnits += h.note.size();
        loadedDetails += h.detailsLoaded;
    }
    require(textUnits == (compact && sqlite ? 0 : manifest["text_utf16_units"].toVariant().toLongLong()) &&
            chapterUnits == (compact && sqlite ? 0 : manifest["chapter_utf16_units"].toVariant().toLongLong()) &&
            noteUnits == (compact && sqlite ? 0 : manifest["note_utf16_units"].toVariant().toLongLong()) &&
            loadedDetails == (compact && sqlite ? 0 : store.highlights().size()), "Unexpected retained memory probe payload");
    QJsonObject result{{"mode", "fresh-process-store-memory"}, {"load_mode", args[3]}, {"qt", qVersion()},
        {"fixture", manifest}, {"resident_before_bytes", before}, {"resident_after_bytes", after},
        {"resident_delta_bytes", after - before}, {"load_wall_ms", wall}, {"load_process_cpu_ms", cpu},
        {"retained_text_utf16_units", textUnits}, {"retained_chapter_utf16_units", chapterUnits},
        {"retained_note_utf16_units", noteUnits}, {"records_with_details", loadedDetails},
        {"scope", "One load in a fresh process, including store construction. RSS includes Qt and allocator/SQLite state; retained units count logical store fields, not all allocations. No fixture payload parsing, book loading, bundled-font registration or reader initialization in this process. Repeat with separate processes to measure variation."}};
    require(storage::writeJson(args[4], result), "Cannot save memory measurements");
}
}
int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "minimal:enable_fonts:freetype");
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setQuitOnLastWindowClosed(false);
    try {
        const auto args = app.arguments();
        if (args.value(1) == "--store-memory-seed") {
            seedMemoryStore(args);
            return 0;
        }
        if (args.value(1) == "--store-memory") {
            measureMemoryStore(args);
            return 0;
        }
        require(args.size() >= 5 && args.size() <= 7, "Expected BOOK FIXTURE COUNT OUTPUT [--sqlite] [--bookmarks]");
        const auto options = args.mid(5);
        for (const auto &option : options)
            require((option == "--sqlite" || option == "--bookmarks") && options.count(option) == 1,
                    "Unknown or repeated option " + option);
        const bool sqlite = options.contains("--sqlite"), bookmarksMode = options.contains("--bookmarks");
        qputenv("KOBALT_ANNOTATIONS_SQLITE", sqlite ? "1" : "0");
        const QString book = QFileInfo(args[1]).absoluteFilePath(), output = args[4];
        const int count = args[3].toInt();
        require(count >= 0 && count <= 2000, "Invalid record count");
        const auto fixture = QJsonDocument::fromJson(read(args[2])).object();
        require(fixture["sha256"].toString().toLatin1() == QCryptographicHash::hash(read(book), QCryptographicHash::Sha256).toHex(),
                "Wrong fixture revision");
        const auto marks = fixtureHighlights(fixture, count);
        AssetBundle assets(QStringLiteral(KOBALT_TEST_ASSET_ROOT), QStringLiteral(KOBALT_VERSION));
        FontCopies copies;
        require(assets.available() && ui::initializeInterfaceFonts(assets), "Cannot initialize fonts");
        for (const auto &path : bundledFontFiles(assets))
            require(QFontDatabase::addApplicationFont(path) >= 0, "Cannot register font");
        readingPpi.store(300);
        ui::setDensity(300);
        app.setFont(ui::interfaceFont());
        const auto fonts = engineFontInputs(assets, {}, {});
        const auto placeholder = placeholderFontFamily(assets, copies);
        QTemporaryDir profile(QFileInfo(output).absolutePath() + "/gui-profile-XXXXXX");
        require(profile.isValid(), "Cannot create isolated profile");
        storage::Paths paths{profile.path()};
        const QString identity = QUrl::fromLocalFile(book).toString();
        QVector<Bookmark> bookmarks;
        for (const auto &mark : marks)
            bookmarks.append({mark.start, mark.chapter, mark.text.left(512), mark.created});
        auto settings = fixture["settings"].toObject();
        settings["book"] = identity;
        settings["schema"] = 1;
        settings["position"] = fixture["start_position"];
        require(storage::writeJson(paths.preferences(), {{"highlight_help_seen", true}, {"last_text_layout", settings}}),
                "Cannot write preferences");
        QJsonObject migration;
        if (sqlite) {
            const QString root = profile.path() + "/migration-annotations";
            qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
            HighlightStore legacy(root, identity, fixture["fingerprint"].toString().toLatin1(), "War and Peace");
            legacy.setHighlights(marks);
            require(legacy.save(), "Cannot seed migration highlights");
            BookmarkStore legacyBookmarks(root, identity, fixture["fingerprint"].toString().toLatin1(), "War and Peace");
            legacyBookmarks.setBookmarks(bookmarks);
            require(legacyBookmarks.save(), "Cannot seed migration bookmarks");
            const auto original = read(legacy.folder() + "/highlights.json");
            const auto originalBookmarks = read(legacy.folder() + "/bookmarks.json");
            qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
            QElapsedTimer timer;
            timer.start();
            const auto begin = cpuMs();
            HighlightStore migrated(root, identity, fixture["fingerprint"].toString().toLatin1(), "War and Peace");
            require(migrated.load(), "Migration load failed");
            migration = {{"highlights", count}, {"bookmarks", count},
                         {"wall_ms", timer.nsecsElapsed()/1e6}, {"cpu_ms", cpuMs()-begin}};
            require(migrated.highlights().size() == marks.size(), "Migration lost highlights");
            for (int i = 0; i < marks.size(); ++i)
                require(highlightJson(migrated.highlights()[i]) == highlightJson(marks[i]), "Migration changed a highlight");
            BookmarkStore migratedBookmarks(root, identity, fixture["fingerprint"].toString().toLatin1(), "War and Peace");
            require(migratedBookmarks.load() && migratedBookmarks.bookmarks() == bookmarks, "Migration changed bookmarks");
            require(read(legacy.folder() + "/highlights.json") == original &&
                    read(legacy.folder() + "/bookmarks.json") == originalBookmarks, "Migration changed original JSON");
        }
        desktop::Window window(paths, fonts, placeholder, [] { return QStringLiteral("09:00"); });
        window.resize(920, 1136);
        auto *resolution = window.findChild<QComboBox *>("desktopResolutionPicker");
        require(resolution, "Missing resolution picker");
        resolution->setCurrentIndex(resolution->findData(300));
        window.show();
        auto widget = [&](const char *name) -> QWidget * {
            return window.readerWindow() ? window.readerWindow()->findChild<QWidget *>(name) : nullptr;
        };
        auto press = [&](const char *name) {
            auto *button = qobject_cast<QPushButton *>(widget(name));
            require(button && button->isVisible() && button->isEnabled(), "Unavailable action " + QString(name));
            button->click();
        };
        bool opened = false;
        QString failure;
        QObject::connect(&window, &desktop::Window::bookOpened, &app, [&] { opened = true; });
        QObject::connect(&window, &desktop::Window::bookFailed, &app, [&](const QString &error) { failure = error; });
        QElapsedTimer heartbeatClock;
        heartbeatClock.start();
        qint64 lastBeat = 0, maxGap = 0;
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] {
            const auto now = heartbeatClock.nsecsElapsed();
            maxGap = qMax(maxGap, now - lastBeat);
            lastBeat = now;
        });
        heartbeat.start(5);
        QVector<double> opens, openCpu, turns, settledTurns, lists, listPaints, warmLists, deletes, gaps;
        auto settleEngine = [&] {
            // Drain worker replies, then any work those replies queued. This
            // lets endpoint lookup complete without a new page revision.
            for (int pass = 0; pass < 2; ++pass) {
                bool drained = false;
                QMetaObject::invokeMethod(engineWorker(), [&] {
                    QMetaObject::invokeMethod(&app, [&] { drained = true; }, Qt::QueuedConnection);
                }, Qt::QueuedConnection);
                waitFor([&] { return drained; }, "engine barrier");
            }
        };
        QJsonArray runDetails;
        qint64 openedRss = 0, listRss = 0;
        int countAfterMerge = count;
        for (int run = 0; run < 6; ++run) {
            {
                HighlightStore store(paths.annotations(), identity, fixture["fingerprint"].toString().toLatin1(), "War and Peace", paths.database());
                require(store.load(), "Cannot initialize highlight store");
                store.setHighlights(bookmarksMode ? QVector<Highlight>() : marks);
                require(store.save(), "Cannot seed highlights");
            }
            if (bookmarksMode) {
                BookmarkStore seeded(paths.annotations(), identity, fixture["fingerprint"].toString().toLatin1(), "War and Peace", paths.database());
                require(seeded.load(), "Cannot initialize bookmark store");
                seeded.setBookmarks(bookmarks);
                require(seeded.save(), "Cannot seed bookmarks");
            }
            require(storage::writeJson(paths.bookState(identity), settings), "Cannot reset reading position");
            opened = false;
            failure.clear();
            maxGap = 0;
            lastBeat = heartbeatClock.nsecsElapsed();
            QElapsedTimer timer;
            const double cpuStart = cpuMs();
            timer.start();
            window.openBook(book);
            waitFor([&] { return opened || !failure.isEmpty(); }, "book opening");
            require(failure.isEmpty(), failure);
            const double open = timer.nsecsElapsed()/1e6, cpu = cpuMs()-cpuStart;
            auto *page = dynamic_cast<ReadingPage *>(widget("rrPage"));
            require(page && !page->image.isNull(), "No reading frame");
            openedRss = residentBytes();
            // Turn the same twenty adjacent pages at every population.
            for (int turn = 0; turn < 20; ++turn) {
                const auto key = page->image.cacheKey();
                timer.restart();
                window.readerWindow()->pageButton(1);
                waitFor([&] { return page->image.cacheKey() != key; }, "page turn");
                if (run > 0) turns.append(timer.nsecsElapsed()/1e6);
            }
            // Return through the same pages after pending endpoint lookup has
            // completed. Keep that waiting outside the per-turn measurement.
            for (int turn = 0; turn < 20; ++turn) {
                settleEngine();
                const auto key = page->image.cacheKey();
                timer.restart();
                window.readerWindow()->pageButton(-1);
                waitFor([&] { return page->image.cacheKey() != key; }, "settled page turn");
                if (run > 0) settledTurns.append(timer.nsecsElapsed()/1e6);
            }
            settleEngine();
            if (bookmarksMode) {
                // The production viewport merges bookmarks on the same page.
                // Read its persisted result outside the UI/list measurement.
                BookmarkStore merged(paths.annotations(), identity, fixture["fingerprint"].toString().toLatin1(), "War and Peace", paths.database());
                require(merged.load(), "Cannot read merged bookmark count");
                countAfterMerge = merged.bookmarks().size();
                require(countAfterMerge <= count && (!count || countAfterMerge > 0), "Invalid merged bookmark count");
            }
            const auto caption = [&](int records) {
                if (bookmarksMode) return strings::get("bookmarks.summary").arg(records);
                return records == 1 ? QStringLiteral("1 annotation") : QString::number(records) + " annotations";
            };
            // Opening the controls is outside the list timing.
            QMouseEvent tap(QEvent::MouseButtonRelease, QPointF(page->width()/2, page->height()/2),
                            QPointF(), QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QMouseEvent down(QEvent::MouseButtonPress, QPointF(page->width()/2, page->height()/2),
                             QPointF(), QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(page, &down);
            QApplication::sendEvent(page, &tap);
            waitFor([&] { auto *b=widget("rrBookmarks"); return b && b->isVisible(); }, "reading controls");
            timer.restart();
            press("rrBookmarks");
            waitFor([&] { auto *w=widget("rrBookmarkList"); return w && w->isVisible(); }, "annotation list");
            if (run > 0) listPaints.append(timer.nsecsElapsed()/1e6);
            auto listReady = [&] {
                auto *sort = widget("rrMarksSort");
                auto *summary = qobject_cast<QLabel *>(widget("rrBookmarkListSummary"));
                const auto expected = caption(countAfterMerge);
                return sort && sort->isEnabled() && ((!bookmarksMode && !count) || (summary && summary->text() == expected));
            };
            waitFor(listReady, "annotation page resolution");
            const double list = timer.nsecsElapsed()/1e6;
            listRss = residentBytes();
            if (run == 0) {
                const auto capture = QFileInfo(output).absolutePath() + '/' + QFileInfo(output).completeBaseName() + "-list.png";
                require(window.grab().save(capture), "Cannot save list evidence");
            }
            press("rrBookmarkListClose");
            waitFor([&] { auto *w=widget("rrBookmarks"); return w && w->isVisible(); }, "reading controls");
            timer.restart();
            press("rrBookmarks");
            waitFor(listReady, "cached annotation list");
            if (run > 0) warmLists.append(timer.nsecsElapsed()/1e6);
            if (countAfterMerge) {
                auto *summary = qobject_cast<QLabel *>(widget("rrBookmarkListSummary"));
                require(summary && summary->text() == caption(countAfterMerge), "Annotation/bookmark list lost records");
                press(bookmarksMode ? "rrBookmarkRemove0" : "rrAnnotationRemove0");
                timer.restart();
                press(bookmarksMode ? "rrConfirmRemoveBookmark" : "rrConfirmRemoveAnnotation");
                waitFor([&] { return summary->text() == caption(countAfterMerge - 1); }, "remove annotation/bookmark");
                if (run > 0) deletes.append(timer.nsecsElapsed()/1e6);
            }
            press("rrBookmarkListClose");
            QCoreApplication::processEvents();
            if (run > 0) {
                opens.append(open);
                openCpu.append(cpu);
                lists.append(list);
                gaps.append(maxGap/1e6);
            }
            runDetails.append(QJsonObject{{"run", run}, {"warmup", run == 0}, {"open_ms", open},
                {"open_cpu_ms", cpu}, {"list_ms", list}, {"max_gui_gap_ms", maxGap/1e6},
                {"count_initial", count}, {"count_after_merge", countAfterMerge},
                {"page_width", page->image.width()}, {"page_height", page->image.height()}});
            window.readerWindow()->close();
            waitFor([&] { return window.idle(); }, "reader shutdown");
            fprintf(stderr, "count=%d run=%d open=%.2f list=%.2f RSS=%lld\n", count, run, open, list, openedRss);
        }
        heartbeat.stop();
        window.close();
        QJsonObject result{{"mode", "production-desktop-headless"}, {"qt", qVersion()}, {"count", count},
            {"record_kind", bookmarksMode ? "bookmarks" : "highlights"},
            {"count_initial", count}, {"count_after_merge", countAfterMerge},
            {"backend", sqlite ? "sqlite" : "json"},
            {"migration", migration},
            {"open", distribution(opens)}, {"open_process_cpu", distribution(openCpu)},
            {"turns", distribution(turns)}, {"list", distribution(lists)}, {"gui_gap", distribution(gaps)},
            {"list_first_paint", distribution(listPaints)}, {"list_warm", distribution(warmLists)},
            {"settled_turns", distribution(settledTurns)},
            {"resident_after_open_bytes", openedRss}, {"resident_after_list_bytes", listRss},
            {"runs", runDetails}, {"book_sha256", fixture["sha256"]},
            {"fixture_sha256", QString::fromLatin1(QCryptographicHash::hash(read(args[2]), QCryptographicHash::Sha256).toHex())},
            {"scope", "Debug production desktop implementation. First opening excluded; five reopens with cached document. Twenty immediate forward turns, then twenty backward turns with pending engine jobs drained before each turn outside timing. Same initial anchor across populations; isolated profile. GUI gap includes all operations; 1ms polling floor. No physical Kobo timing."}};
        if (!deletes.isEmpty()) result["delete"] = distribution(deletes);
        {
            HighlightStore store(paths.annotations(), identity, fixture["fingerprint"].toString().toLatin1(), "War and Peace", paths.database());
            BookmarkStore bookmarkStore(paths.annotations(), identity, fixture["fingerprint"].toString().toLatin1(), "War and Peace", paths.database());
            if (bookmarksMode) {
                require(bookmarkStore.load(), "Cannot open measured bookmark store");
                bookmarkStore.setBookmarks(bookmarks);
                require(bookmarkStore.save(), "Cannot restore original unmerged bookmark population");
            } else {
                require(store.load(), "Cannot open measured store");
                store.setHighlights(marks);
                require(store.save(), "Cannot reset measured store");
            }
            QVector<double> loads, loadCpu, edits, editCpu;
            for (int i = 0; i < 22; ++i) {
                QElapsedTimer timer;
                double beginCpu = cpuMs();
                timer.start();
                require(bookmarksMode ? bookmarkStore.load() : store.load(), "Measured store load failed");
                const double loadWallMs = timer.nsecsElapsed()/1e6;
                const double loadCpuMs = cpuMs() - beginCpu;
                if (i) { loads.append(loadWallMs); loadCpu.append(loadCpuMs); }
                if (count) {
                    if (bookmarksMode) {
                        require(bookmarkStore.bookmarks().size() == count, "Measured bookmark population changed");
                        auto changed = bookmarkStore.bookmarks();
                        changed[count/2].chapter = i % 2 ? "Benchmark chapter A" : "Benchmark chapter B";
                        beginCpu = cpuMs();
                        timer.restart();
                        bookmarkStore.setBookmarks(std::move(changed));
                        require(bookmarkStore.save(), "Measured bookmark edit failed");
                    } else {
                        auto changed = store.highlights();
                        changed[count/2].color = i % 2 ? "yellow" : "blue";
                        beginCpu = cpuMs();
                        timer.restart();
                        store.setHighlights(std::move(changed));
                        require(store.save(), "Measured store edit failed");
                    }
                    const double editWallMs = timer.nsecsElapsed()/1e6;
                    const double editCpuMs = cpuMs() - beginCpu;
                    if (i) { edits.append(editWallMs); editCpu.append(editCpuMs); }
                }
            }
            result["production_store_load"] = distribution(loads);
            result["production_store_load_process_cpu"] = distribution(loadCpu);
            result["production_store_count"] = count;
            if (count) {
                result["production_store_edit"] = distribution(edits);
                result["production_store_edit_process_cpu"] = distribution(editCpu);
            }
        }
        if (!bookmarksMode) {
            // The full-load baseline above has released its store. Use the same
            // compact API as the production session, including JSON's fallback.
            HighlightStore compact(paths.annotations(), identity, fixture["fingerprint"].toString().toLatin1(), "War and Peace", paths.database());
            QStringList excerptIds;
            QVector<int> excerptIndices;
            for (int i = 0; i < qMin(count, 5); ++i) {
                const int index = count - 1 - i;
                excerptIndices.append(index);
                excerptIds.append(marks[index].id);
            }
            const int detailIndex = count / 2;
            QVector<double> loads, loadCpu, excerpts, excerptCpu, details, detailCpu, edits, editCpu;
            for (int i = 0; i < 22; ++i) {
                QElapsedTimer timer;
                double beginCpu = cpuMs();
                timer.start();
                require(compact.loadCompact(), "Measured compact load failed");
                const double loadWallMs = timer.nsecsElapsed()/1e6;
                const double loadCpuMs = cpuMs() - beginCpu;
                if (i) { loads.append(loadWallMs); loadCpu.append(loadCpuMs); }
                require(compact.highlights().size() == count, "Compact population changed");
                for (const auto &h : compact.highlights())
                    require(sqlite ? !h.detailsLoaded && h.text.isEmpty() && h.chapter.isEmpty() && h.note.isEmpty()
                                   : h.detailsLoaded,
                            "Unexpected compact payload retention");
                if (!count)
                    continue;
                QVector<HighlightExcerpt> pageExcerpts;
                beginCpu = cpuMs();
                timer.restart();
                require(compact.excerpts(excerptIds, &pageExcerpts), "Measured excerpt batch failed");
                const double excerptWallMs = timer.nsecsElapsed()/1e6;
                const double excerptCpuMs = cpuMs() - beginCpu;
                if (i) { excerpts.append(excerptWallMs); excerptCpu.append(excerptCpuMs); }
                require(pageExcerpts.size() == excerptIds.size(), "Excerpt batch lost records");
                for (int j = 0; j < pageExcerpts.size(); ++j) {
                    const auto &expected = marks[excerptIndices[j]];
                    const auto &actual = pageExcerpts[j];
                    require(actual.id == expected.id && actual.text == expected.text && actual.chapter == expected.chapter,
                            "Excerpt batch changed order or payload");
                }
                QVector<Highlight> detail;
                beginCpu = cpuMs();
                timer.restart();
                require(compact.details({marks[detailIndex].id}, &detail), "Measured full-detail load failed");
                const double detailWallMs = timer.nsecsElapsed()/1e6;
                const double detailCpuMs = cpuMs() - beginCpu;
                if (i) { details.append(detailWallMs); detailCpu.append(detailCpuMs); }
                require(detail.size() == 1 && detail[0].detailsLoaded && detail[0].id == marks[detailIndex].id &&
                        detail[0].text == marks[detailIndex].text && detail[0].chapter == marks[detailIndex].chapter &&
                        detail[0].note == marks[detailIndex].note, "Full-detail load changed payload");
                auto changed = compact.highlights();
                changed[detailIndex].color = i % 2 ? "yellow" : "blue";
                beginCpu = cpuMs();
                timer.restart();
                compact.setHighlights(std::move(changed));
                require(compact.save(), "Measured compact appearance edit failed");
                const double editWallMs = timer.nsecsElapsed()/1e6;
                const double editCpuMs = cpuMs() - beginCpu;
                if (i) { edits.append(editWallMs); editCpu.append(editCpuMs); }
                detail.clear();
                require(compact.details({marks[detailIndex].id}, &detail) && detail.size() == 1 &&
                        detail[0].note == marks[detailIndex].note && detail[0].text == marks[detailIndex].text &&
                        detail[0].chapter == marks[detailIndex].chapter &&
                        detail[0].color == (i % 2 ? "yellow" : "blue"), "Compact edit changed retained payload");
            }
            result["production_store_compact_load"] = distribution(loads);
            result["production_store_compact_load_process_cpu"] = distribution(loadCpu);
            result["production_store_compact_is_full_fallback"] = !sqlite;
            result["production_store_excerpt_batch_count"] = excerptIds.size();
            if (count) {
                result["production_store_excerpt_batch"] = distribution(excerpts);
                result["production_store_excerpt_batch_process_cpu"] = distribution(excerptCpu);
                result["production_store_full_detail"] = distribution(details);
                result["production_store_full_detail_process_cpu"] = distribution(detailCpu);
                result["production_store_compact_edit"] = distribution(edits);
                result["production_store_compact_edit_process_cpu"] = distribution(editCpu);
            }
            qint64 noteUnits = 0;
            for (const auto &h : marks)
                noteUnits += h.note.size();
            result["fixture_note_utf16_units"] = noteUnits;
        }
        qint64 annotationBytes = 0;
        int annotationFiles = 0;
        QDirIterator files(paths.annotations(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            files.next();
            annotationBytes += files.fileInfo().size();
            ++annotationFiles;
        }
        if (QFileInfo::exists(paths.database())) {
            annotationBytes += QFileInfo(paths.database()).size();
            ++annotationFiles;
        }
        result["annotation_storage_file_bytes"] = annotationBytes;
        result["annotation_storage_files"] = annotationFiles;
        require(storage::writeJson(output, result), "Cannot save measurements");
        // Production workers drain through aboutToQuit, as in the desktop tests.
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
        return app.exec();
    } catch (const std::exception &error) {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
