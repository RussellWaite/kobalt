#include "reader_inputs.h"
#include "check.h"
#include "reading/reading_book.h"
#include "reading/appearance_settings.h"
#include "interfaces/opening_message.h"
#include "reading/reader_session.h"
#include "storage/reader_storage.h"
#include "storage/annotation_database.h"
#include <QSqlQuery>
#include "publication/native_resources.h"
#include "support/qt_compat.h"
#include "publication/resource_server.h"
#include "publication/native_archive.h"
#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <utility>

using namespace kobalt;
namespace {
QJsonObject persistedBook(QJsonObject state) {
    for (const auto *key : appearance::layoutKeys)
        state.remove(key);
    for (const auto *key : appearance::marginKeys)
        state.remove(key);
    return state;
}
void waitUntil(const std::function<bool()> &ready) {
    if (ready())
        return;
    QEventLoop loop;
    QTimer poll, timeout;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (ready())
            loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [] {
        require(false, "ReadingBook check timed out");
    });
    poll.start(1);
    timeout.start(60000);
    loop.exec();
}
void tick(int milliseconds = 10) {
    QEventLoop loop;
    qt::later(milliseconds, &loop, [&] {
        loop.quit();
    });
    loop.exec();
}
struct Calls {
    int reads = 0, writes = 0, observed = 0, stopped = 0, destroyed = 0;
    bool writable = true, transition = false, changed = true, failWrite = false;
    bool finished = false;
    QString location;
    ReadingProgress current{"store-id", "chapter.xhtml#one", 1, 1, true};
    ReadingEstimates estimate;
    ReadingActivity writtenActivity;
    bool providesEstimates = false;
};
class Bridge final : public ReadingBridge {
public:
    explicit Bridge(Calls &calls) : calls(calls) {}
    ~Bridge() override {
        ++calls.destroyed;
    }
    bool deviceTransition() const override {
        return calls.transition;
    }
    bool writable() const override {
        return calls.writable && !calls.transition;
    }
    ReadingProgress read(QString *) const override {
        ++calls.reads;
        return calls.current;
    }
    bool takeChange() override {
        return std::exchange(calls.changed, false);
    }
    bool write(const ReadingProgress &expected, const QString &location, double, bool finished,
               const ReadingActivity &activity, ReadingProgress *verified, QString *error) override {
        ++calls.writes;
        calls.writtenActivity = activity;
        calls.finished = finished;
        calls.location = location;
        require(expected == calls.current, "Bridge received a stale expected baseline");
        if (calls.failWrite) {
            *error = "synthetic write failure";
            return false;
        }
        calls.current.location = location;
        ++calls.current.modified;
        calls.current.status = finished ? 2 : 1;
        *verified = calls.current;
        return true;
    }
    void observeReading(const ReadingActivity &) override {
        ++calls.observed;
    }
    bool estimates(const ReadingActivity &, ReadingEstimates *result) override {
        *result = calls.estimate;
        return calls.providesEstimates;
    }
    void stopReading() override {
        ++calls.stopped;
    }

private:
    Calls &calls;
};
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    {
        // Replay the Clara BW report: 307 seconds of reading included 223
        // seconds of kernel standby that CLOCK_MONOTONIC did not count.
        qint64 boot = 44661;
        ReadingTimer timer([&] { return boot; });
        require(!timer.isValid() && timer.elapsed() == 0, "Reading timer started before reading");
        timer.start();
        boot = 352003;
        require(timer.includesStandby() && timer.elapsed() == 307342,
                "Reading timer lost standby time from the Clara BW report");
        const auto first = timer.elapsed();
        timer.invalidate();
        boot += 600000;
        require(!timer.isValid() && timer.elapsed() == 0, "Paused reading timer kept counting");
        timer.start();
        boot += 60000;
        require(first + timer.elapsed() == 367342, "Resuming counted deliberate sleep");
        boot = -1;
        require(timer.elapsed() == 60000, "A failed clock read changed the accumulated duration");
        boot = 352003 + 600000 + 90000;
        require(timer.elapsed() == 90000, "Reading clock did not recover in its original time domain");
        boot -= 1000;
        require(timer.elapsed() == 90000, "A backward clock sample reduced reading time");
        timer.start();
        boot += 1000;
        require(timer.elapsed() == 1000, "A new segment retained the previous duration");
        boot = -1;
        timer.start();
        require(!timer.includesStandby() && timer.elapsed() >= 0,
                "An unavailable boot clock did not select the segment fallback");
        boot = 1000000000;
        require(!timer.includesStandby() && timer.elapsed() < 60000,
                "A recovering clock changed origins halfway through a fallback segment");
    }
    require(argc == 2, "Expected a synthetic EPUB");
    const auto path = QString::fromLocal8Bit(argv[1]);
    native::ResourceServer server;
    require(server.open(path,
                        [path] {
                            return native::openPlainArchive(path);
                        }),
            "Cannot open synthetic publication");
    QTemporaryDir temporary;
    require(temporary.isValid(), "Cannot create state directory");
    storage::Paths paths{temporary.path()};
    const auto open = [&](QSize size = QSize(758, 900)) {
        auto session = std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
        bool ready = false;
        auto lease = std::make_shared<SourceLease>();
        session->open({"test-book", path, size, {}, {}, {}, lease, nativeResourceFactory(path, server.port(), lease)},
                      [&](const OpeningResult &result) {
                          require(result.ready(), qPrintable(openingMessage(result)));
                          ready = true;
                      });
        waitUntil([&] {
            return ready;
        });
        return session;
    };
    const auto historyValue = [&](const QString &statement) {
        QVariant value;
        require(AnnotationDatabase::transaction(paths.directory, [&](QSqlDatabase &db) {
            QSqlQuery query(db);
            if (!query.exec(statement) || !query.next()) return false;
            value = query.value(0);
            return true;
        }), "Cannot inspect reading history");
        return value;
    };
    for (const auto &original : {qt::object({{"line_spacing", 122}}), QJsonObject(),
                                 qt::object({{"font_pixels", 31},
                                             {"font_face", "Libron"},
                                             {"line_spacing", QJsonValue(QJsonValue::Null)},
                                             {"dark_mode", true}})}) {
        auto migratedOriginal = original;
        appearance::migrateLineSpacing(migratedOriginal);
        const QString identity = "file:///mnt/onboard/kobalt-benchmark.kepub.epub";
        auto session = std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
        bool ready = false;
        auto lease = std::make_shared<SourceLease>();
        OpenRequest request{identity, path, QSize(758, 900), original,
                            {},       {},   lease,           nativeResourceFactory(path, server.port(), lease)};
        request.temporaryAppearance = appearance::benchmarkTypography(identity);
        request.originalAppearance = original;
        session->open(std::move(request), [&](const OpeningResult &result) {
            require(result.ready(), qPrintable(openingMessage(result)));
            ready = true;
        });
        waitUntil([&] {
            return ready;
        });
        require(session->temporaryTypography() && session->fontFace().isEmpty() &&
                    session->fontSize() == defaultFontPixels() && session->margin() == readingPixels(60) &&
                    session->topMargin() == readingPixels(60) && !session->state().value("dark_mode").toBool(),
                "Temporary appearance was lost during cold opening");
        require(!session->style(30, 100, "Libron", 0, "left") && !session->setWeight(2) &&
                    !session->setHyphenation("on") && !session->setVerticalMargins(0),
                "Temporary appearance accepted a typography edit");
        session->setDarkMode(true);
        session->apply(original);
        waitUntil([&] {
            return !session->busy();
        });
        const auto hash = session->sourceHash();
        const auto statePath = paths.bookState(identity);
        std::unique_ptr<ReaderSession> retained;
        {
            ReadingBook benchmark(std::move(session), statePath, paths, identity, hash, nullptr, {}, {});
            QJsonObject snapshot;
            require(benchmark.checkpoint(&snapshot), "Benchmark checkpoint failed");
            require(snapshot.value("font_pixels").toInt() == defaultFontPixels(),
                    "Live snapshot lost benchmark appearance");
            const auto persisted = storage::readBookState(paths, identity, statePath).object;
            require(appearance::same(persisted, migratedOriginal),
                    "Checkpoint persisted temporary appearance or added absent keys");
            require(benchmark.session()->turn(1), "Benchmark cannot turn");
            waitUntil([&] {
                return !benchmark.session()->busy();
            });
            require(benchmark.checkpoint(&snapshot), "Benchmark turn checkpoint failed");
            require(appearance::same(storage::readBookState(paths, identity, statePath).object, migratedOriginal),
                    "Turn checkpoint changed original appearance");
            require(!storage::readJson(paths.preferences()).contains(appearance::lastTextLayoutKey),
                    "Temporary benchmark typography became the next book's style");
            retained = benchmark.session()->retained();
            require(bool(retained), "Benchmark could not retain its parsed document");
        }
        require(appearance::same(storage::readBookState(paths, identity, statePath).object, migratedOriginal),
                "Closing changed original appearance");
        ready = false;
        lease = std::make_shared<SourceLease>();
        retained->open({identity,
                        path,
                        QSize(758, 900),
                        storage::readBookState(paths, identity, statePath).object,
                        {},
                        {},
                        lease,
                        nativeResourceFactory(path, server.port(), lease)},
                       [&](const OpeningResult &result) {
                           require(result.ready(), qPrintable(openingMessage(result)));
                           ready = true;
                       });
        waitUntil([&] {
            return ready;
        });
        require(!retained->temporaryTypography() &&
                    retained->fontSize() == original.value("font_pixels").toInt(defaultFontPixels()) &&
                    retained->spacing() == migratedOriginal.value("line_spacing").toInt(defaultLineSpacing),
                "Normal reopening retained temporary typography");
        auto ordinary = open();
        require(!ordinary->temporaryTypography(), "Unmatched opening inherited benchmark typography");
    }
    {
        auto session = open();
        const auto hash = session->sourceHash();
        require(storage::writeJson(paths.preferences(),
                                   qt::object({{"show_clock", false}, {"header_text_size", "large"}, {"orientation", 2}})),
                "Cannot save global layout");
        ReadingBook local(std::move(session), paths.bookState("local-id"), paths, "local-id", hash, nullptr, {}, {});
        require(local.saveDictionaryChoice("test-dictionary"), "Cannot save dictionary choice");
        QJsonObject layoutSnapshot;
        require(local.checkpoint(&layoutSnapshot), "Cannot checkpoint book layout");
        const auto savedLayout = storage::readBookState(paths, "local-id").object;
        require(!savedLayout.contains("show_clock") && !savedLayout.contains("header_text_size") &&
                    !savedLayout.contains("orientation") && !savedLayout.contains("margin_pixels"),
                "Checkpoint persisted global layout with a book");
        require(savedLayout["dictionary_id"].toString() == "test-dictionary", "Checkpoint lost dictionary choice");
        require(storage::readJson(paths.preferences()).value("orientation").toInt() == 2 &&
                    !layoutSnapshot.value("show_clock").toBool(),
                "Checkpoint lost shared rotation or layout inspection");
        require(local.session()->setVerticalMargins(readingPixels(36)), "Cannot change shared vertical margins");
        waitUntil([&] { return !local.session()->busy(); });
        require(local.checkpoint(&layoutSnapshot), "Cannot checkpoint changed global margins");
        require(storage::readJson(paths.preferences()).value(appearance::pageMarginsKey).toObject()
                    .value("top_margin_pixels").toInt() == 36,
                "A margins-only change was skipped by checkpoint caching");
        require(!local.canWriteNative() && !local.deviceTransition(), "Local book acquired platform services");
        require(local.estimates().learningPace && local.estimates().bookMilliseconds > 0,
                "Standalone book did not provide a rough estimate at opening");
        local.pollProgress();
        local.observeProgress();
        require(!local.readProgress().valid && !local.pendingProgress(), "Local book invented external progress");
        local.observeReading();
        tick();
        local.stopReading();
        auto active = local.readingMilliseconds();
        require(active > 0, "Local reading time did not run without a bridge");
        require(local.tracksReadingHistory() &&
                historyValue("SELECT SUM(active_ms) FROM reading_sessions WHERE identity='local-id'").toLongLong() == active &&
                historyValue("SELECT COUNT(*) FROM reading_sessions WHERE identity='local-id' AND status='completed'").toInt() == 1,
                "Local stop did not persist reading history");
        local.observeReading();
        tick();
        require(local.checkpointReadingHistory(), "Active interval checkpoint failed");
        QJsonObject unchangedCheckpoint;
        require(local.checkpoint(&unchangedCheckpoint), "Initial unchanged-checkpoint fixture");
        require(AnnotationDatabase::transaction(paths.directory, [](QSqlDatabase &db) {
            QSqlQuery query(db);
            return query.exec("CREATE TABLE history_write_audit(value INTEGER)") &&
                   query.exec("CREATE TRIGGER audit_history AFTER UPDATE ON reading_sessions BEGIN INSERT INTO history_write_audit VALUES(1); END");
        }), "History write audit fixture");
        tick();
        require(local.checkpoint(&unchangedCheckpoint) &&
                historyValue("SELECT COUNT(*) FROM history_write_audit").toInt() == 0,
                "Unchanged checkpoint rewrote reading history");
        require(local.checkpointReadingHistory() &&
                historyValue("SELECT COUNT(*) FROM history_write_audit").toInt() == 1,
                "Periodic checkpoint skipped time without a page turn");
        require(AnnotationDatabase::transaction(paths.directory, [](QSqlDatabase &db) {
            QSqlQuery query(db);
            return query.exec("DROP TRIGGER audit_history") && query.exec("DROP TABLE history_write_audit");
        }), "Remove history write audit");

        require(AnnotationDatabase::transaction(paths.directory, [](QSqlDatabase &db) {
            QSqlQuery query(db);
            return query.exec("CREATE TRIGGER fail_history BEFORE UPDATE ON reading_sessions BEGIN SELECT RAISE(ABORT,'test'); END");
        }), "History failure fixture");
        require(!local.checkpointReadingHistory(), "Blocked history update succeeded");
        QJsonObject independentCheckpoint;
        require(local.checkpoint(&independentCheckpoint), "History failure blocked independent progress save");
        require(AnnotationDatabase::transaction(paths.directory, [](QSqlDatabase &db) {
            QSqlQuery query(db); return query.exec("DROP TRIGGER fail_history");
        }) && local.checkpointReadingHistory(), "History update retry failed");

        local.stopReading();
        require(historyValue("SELECT COUNT(*) FROM reading_sessions WHERE identity='local-id'").toInt() == 2,
                "Resume did not start a separate interval");
        active = local.readingMilliseconds();

        tick();
        require(local.readingMilliseconds() == active, "Stopped local reading time kept running");
        require(local.session()->go(local.session()->pages() - 1), "Cannot reach the final page");
        waitUntil([&] {
            return !local.session()->busy();
        });
        require(local.offerFinish(), "Local book cannot be marked finished without a bridge");
        local.markFinished();
        require(!local.offerFinish(), "Local book repeatedly offered the same finish position");
        require(local.writeNative({}, "no native mapping"), "Local-only save required a platform position");
        QJsonObject snapshot;
        require(local.checkpoint(&snapshot), "Local checkpoint failed");
        require(persistedBook(snapshot) == storage::readBookState(paths, "local-id").object, "Local checkpoint fields changed on disk");
        require(snapshot.value("book") == QStringLiteral("local-id") && snapshot.value("schema") == 1 &&
                    snapshot.value("fingerprint").toString() == QString::fromLatin1(hash) &&
                    !snapshot.value("finished_position").toString().isEmpty() &&
                    snapshot.value("native_writeback") == QStringLiteral("local-only") &&
                    snapshot.value("native_writeback_error").toString().isEmpty(),
                "Local checkpoint lost identity, finish or writeback state");
        local.releaseNative();
        local.observeReading();
        tick();
        require(local.readingMilliseconds() == active && !local.offerFinish(),
                "Released local book became active again");
    }
    {
        auto session = open(QSize(400, 600));
        const auto hash = session->sourceHash();
        ReadingBook first(std::move(session), paths.bookState("pace-first"), paths, "pace-first", hash, nullptr, {},
                          {});
        auto *model = first.session();
        const ReadingActivity activity{model->state().value("position").toString(),
                                       0,
                                       model->page(),
                                       model->pages(),
                                       0,
                                       model->totalWords(),
                                       model->chapterGraph(),
                                       model->graphSelection()};
        const int words = wordEstimates::pageWords(activity);
        require(words >= 2 && words < 1000, "Pace persistence fixture has no usable page sample");
        first.observeReading();
        // The default single-shot timer can wake early. Pace samples need at
        // least two seconds on the book's clock, not a requested timer delay.
        const auto sampleMilliseconds = qMax(2100, words * 60);
        waitUntil([&] { return first.readingMilliseconds() >= sampleMilliseconds; });
        require(model->turn(1), "Pace persistence fixture cannot turn");
        waitUntil([&] {
            return !model->busy();
        });
        first.observeReading();
        first.stopReading();
        const auto saved = storage::readRecord(paths.directory, "reading-speed", paths.readingSpeed()).object;
        require(
            saved.value("samples").toInt() == 1 && saved.value("words").toDouble() == words,
            qPrintable(
                QString(
                    "Stopping did not save accepted reading history: words=%1, elapsed=%2, page=%3, direction=%4, saved=%5")
                    .arg(words)
                    .arg(first.readingMilliseconds())
                    .arg(model->page())
                    .arg(model->turnDirection())
                    .arg(QString::fromUtf8(QJsonDocument(saved).toJson(QJsonDocument::Compact)))));
        ReadingSpeed expected;
        expected.restore(saved);
        auto next = open(QSize(400, 600));
        ReadingBook second(std::move(next), paths.bookState("pace-second"), paths, "pace-second", hash, nullptr, {},
                           {});
        require(second.estimates().bookMilliseconds == expected.estimates(activity).bookMilliseconds &&
                    second.estimates().learningPace,
                "Another book did not reuse saved partial history");
        second.observeReading();
        second.observeReading(false);
        second.stopReading();
        require(storage::readRecord(paths.directory, "reading-speed", paths.readingSpeed()).object == saved, "Unsampled reading overwrote saved history");
    }
    {
        qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook legacy(std::move(session), paths.bookState("legacy-history"), paths, "legacy-history", hash,
                           nullptr, {}, {});
        require(!legacy.tracksReadingHistory(), "Legacy diagnostic backend enabled SQLite history");
        legacy.observeReading();
        tick();
        legacy.stopReading();
        require(historyValue("SELECT COUNT(*) FROM reading_sessions WHERE identity='legacy-history'").toInt() == 0,
                "Legacy diagnostic backend wrote session history");
        qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
    }
    {
        Calls calls;
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook book(std::move(session), paths.bookState("store-id"), paths, "store-id", hash,
                         std::make_unique<Bridge>(calls), calls.current, calls.current);
        book.observeReading();
        require(calls.observed == 1, "Platform activity was not recorded");
        require(!book.tracksReadingHistory() &&
                historyValue("SELECT COUNT(*) FROM reading_sessions WHERE identity='store-id'").toInt() == 0,
                "Native bridge also recorded standalone history");

        calls.providesEstimates = true;
        calls.estimate = ReadingEstimates{12000, 45000, false};
        require(book.estimates().chapterMilliseconds == 12000 && book.estimates().bookMilliseconds == 45000 &&
                    !book.estimates().learningPace,
                "Platform estimates did not replace the session learning delay");
        calls.estimate = ReadingEstimates{};
        require(book.estimates().bookMilliseconds == -1 && !book.estimates().learningPace,
                "Unavailable platform counts became a local learning estimate");
        calls.providesEstimates = false;
        calls.transition = true;
        book.observeReading();
        require(book.deviceTransition() && !book.canWriteNative() && calls.observed == 1 && calls.stopped == 1,
                "Device transition did not stop platform activity");
        require(book.writeNative("position", {}), "Deferred platform write blocked local checkpoint");
        require(calls.writes == 0, "Platform write ran during transition");
        calls.transition = false;
        ++calls.current.modified;
        calls.current.location = "chapter.xhtml#remote";
        book.pollProgress();
        require(book.pendingProgress(), "External progress change did not create a conflict");
        require(!book.writeNative("position", {}) && calls.writes == 0, "Conflict was overwritten without a choice");
        book.acknowledge(book.candidate(), false);
        require(book.session()->turn(1), "Cannot turn before saving native progress");
        waitUntil([&] {
            return !book.session()->busy();
        });
        require(book.writeNative("position", {}) && calls.writes == 1 && calls.location == "position",
                "Acknowledged progress was not written");
        require(calls.observed == 1 && calls.writtenActivity.page == book.session()->page() &&
                    calls.writtenActivity.position == book.session()->state().value("position").toString(),
                "Native estimate used the preceding observed page when saving a completed turn");
        QJsonObject snapshot;
        require(book.checkpoint(&snapshot), "Platform checkpoint failed");
        require(snapshot.value("nickel_progress").toObject() == calls.current.json() &&
                    snapshot.value("native_writeback") == QStringLiteral("verified"),
                "Verified native baseline changed");
        const auto committed = calls.current;
        calls.failWrite = true;
        require(book.writeNative("position-two", {}), "Ordinary platform failure blocked local persistence");
        require(book.checkpoint(&snapshot), "Failed-write local checkpoint failed");
        require(snapshot.value("nickel_progress").toObject() == committed.json() &&
                    snapshot.value("native_writeback") == QStringLiteral("local-only") &&
                    snapshot.value("native_writeback_error") == QStringLiteral("synthetic write failure"),
                "Failed native write changed the confirmed baseline");
        const int writes = calls.writes;
        require(book.writeNative({}, "unmappable store position") && calls.writes == writes,
                "Store book accepted a time-only write");
        const auto changeSql = [&](const QString &sql) {
            return AnnotationDatabase::transaction(paths.directory, [&](QSqlDatabase &db) {
                QSqlQuery query(db); return query.exec(sql);
            });
        };
        require(changeSql("CREATE TRIGGER fail_checkpoint BEFORE UPDATE ON book_state BEGIN SELECT RAISE(ABORT,'test'); END"),
                "Cannot obstruct checkpoint transaction");
        require(!book.checkpoint(&snapshot), "Checkpoint reported success when storage refused the write");
        require(changeSql("DROP TRIGGER fail_checkpoint") && book.checkpoint(&snapshot),
                "Failed checkpoint was cached instead of retried");
        require(storage::readBookState(paths, "store-id").object == persistedBook(snapshot), "Retried checkpoint lost the current state");
        book.releaseNative();
        require(calls.destroyed == 1 && !book.canWriteNative(), "Release retained platform services");
        book.pollProgress();
        book.observeProgress();
        book.observeReading();
        require(calls.destroyed == 1 && !book.offerFinish(), "Released book accessed its bridge again");
    }
    bool drained = false;
    ReaderSession::drain([&] {
        drained = true;
    });
    waitUntil([&] {
        return drained;
    });
    bool disconnected = false;
    releaseResourceConnections([&] {
        disconnected = true;
    });
    waitUntil([&] {
        return disconnected;
    });
    std::puts("PASS: local reading state and optional platform activity, conflict, writeback and release");
    // ReaderSession stops its worker on application shutdown. Nested waits
    // above do not emit aboutToQuit; leave through the application event loop.
    qt::later(0, &app, [&] {
        app.quit();
    });
    return app.exec();
}
