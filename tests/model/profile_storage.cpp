#include "storage/reader_storage.h"
#include "storage/annotation_database.h"
#include "storage/profile_database.h"
#include "storage/bookmark_store.h"
#ifndef KOBALT_TEST_NO_LIBRARY
#include "library/library_index.h"
#endif
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSqlQuery>
#include <QSqlDatabase>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QVariant>
#include <utime.h>

using namespace kobalt;
namespace {
void require(bool condition, const char *message) { if (!condition) qFatal("%s", message); }
QByteArray bytes(const QString &path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Read fixture"); return file.readAll();
}
void write(const QString &path, const QByteArray &value) {
    QFile file(path); require(file.open(QIODevice::WriteOnly) && file.write(value) == value.size(), "Write fixture");
}
// Stands in for a change made while Kobalt was closed: the profile closes
// afterwards, so the next operation opens and checks the file again.
void execute(const QString &directory, const QString &sql) {
    require(AnnotationDatabase::transaction(directory, [&](QSqlDatabase &db) {
        QSqlQuery query(db); return query.exec(sql);
    }), "SQL fixture");
    storage::closeProfile();
}
// Kobalt holds the profile file exclusively while it is open. Close it, as
// USB export does, before probing the file with another connection.
QVariant rawValue(const QString &directory, const QString &sql) {
    storage::closeProfile();
    const auto name = QStringLiteral("profile-schema-probe");
    auto db = QSqlDatabase::addDatabase("QSQLITE", name);
    db.setDatabaseName(directory + "/kobalt.sqlite");
    require(db.open(), "Open raw schema fixture");
    QVariant result;
    { QSqlQuery query(db); require(query.exec(sql) && query.next(), "Query raw schema fixture"); result = query.value(0); }
    db.close(); db = QSqlDatabase(); QSqlDatabase::removeDatabase(name);
    return result;
}


}
int main(int argc, char **argv) {
    if (argc == 2) qputenv("TMPDIR", QByteArray(argv[1]));
    QCoreApplication app(argc, argv);
    using storage::ReadStatus;
    QTemporaryDir root;
    require(root.isValid(), "Temporary profile");
    const storage::Paths paths{root.path()};
    const QString identity = "file:///books/invented.epub";
    QJsonObject state;
    state["schema"] = 1; state["book"] = identity;
    state["position"] = QStringLiteral("/body/section[2]/p[4]/text().12");
    state["font_face"] = QStringLiteral("Publisher"); state["font_pixels"] = 41;
    state["dictionary_id"] = QStringLiteral("custom-fr");
    state["nickel_progress"] = qt::object({{"location", "native-position"}, {"percent", 24}});
    state["future_optional"] = qt::array({1, false, qt::object({{"note", "Preserve me"}})});
    state["literal"] = QString(QChar(0xfeff)) + QString::fromUtf8("é") + QChar(0) + "tail";
    state["null"] = QJsonValue();
    require(storage::writeJson(paths.bookState(identity), state), "Legacy book fixture");
    const auto original = bytes(paths.bookState(identity));
    require(storage::prepareProfile(paths), "Import complete profile");
    require(storage::readBookState(paths, identity).object == state, "Imported state changed");
    const auto backup = root.path() + "/migration-backup/" + QFileInfo(paths.bookState(identity)).fileName();
    require(!QFile::exists(paths.bookState(identity)) && bytes(backup) == original, "Import backup changed");
    auto edited = state; edited["position"] = QStringLiteral("/body/p[50]");
    require(storage::writeBookState(paths, identity, edited), "Save database checkpoint");
    execute(root.path(), "CREATE TRIGGER unchanged_checkpoint BEFORE UPDATE ON book_state BEGIN SELECT RAISE(ABORT,'unchanged'); END");
    require(storage::writeBookState(paths, identity, edited), "Unchanged JSON checkpoint rewrote its row");
    execute(root.path(), "DROP TRIGGER unchanged_checkpoint");
    edited["position"] = QStringLiteral("/body/p[51]");
    require(storage::writeBookState(paths, identity, edited), "Update one JSON checkpoint");
    require(rawValue(root.path(), "SELECT count(*) FROM book_state").toInt() == 1 &&
            rawValue(root.path(), "SELECT revision FROM book_state").toInt() == 2,
            "Book state is not one JSON row per book");
    write(paths.bookState(identity), original);
    require(storage::readBookState(paths, identity).object == edited, "Stale JSON replaced a newer checkpoint");
    require(storage::writeBookState(paths, identity, qt::object({{"book", identity}})) &&
            storage::readBookState(paths, identity).object == qt::object({{"book", identity}}),
            "Mark unread retained old settings or position");
    execute(root.path(), "CREATE TRIGGER fail_checkpoint BEFORE UPDATE ON book_state BEGIN SELECT RAISE(ABORT,'test'); END");
    require(!storage::writeBookState(paths, identity, edited) &&
            storage::readBookState(paths, identity).object == qt::object({{"book", identity}}), "Failed write changed state");
    execute(root.path(), "DROP TRIGGER fail_checkpoint");
    auto oversized = edited; oversized["large"] = QString(int(storage::maxJsonBytes), 'x');
    require(!storage::writeBookState(paths, identity, oversized) &&
            storage::readBookState(paths, identity).object == qt::object({{"book", identity}}), "Oversized book state replaced saved data");
    QTemporaryDir invalidPayload;
    require(storage::writeBookState(storage::Paths{invalidPayload.path()}, identity, state), "Initialize damaged JSON fixture");
    execute(invalidPayload.path(), "UPDATE book_state SET payload='{broken'");
    require(storage::readBookState(storage::Paths{invalidPayload.path()}, identity).status == ReadStatus::Invalid &&
            !storage::writeBookState(storage::Paths{invalidPayload.path()}, identity, edited), "Damaged database JSON was overwritten");
    const auto broken = paths.bookState("broken");
    write(broken, "{ invalid");
    require(storage::readBookState(paths, "broken").status == ReadStatus::Invalid &&
            !storage::writeBookState(paths, "broken", qt::object({{"book", "broken"}})) && bytes(broken) == "{ invalid",
            "Malformed input was replaced");
    const auto future = paths.bookState("future");
    require(storage::writeJson(future, qt::object({{"book", "future"}, {"schema", 99}})), "Future fixture");
    const auto futureBytes = bytes(future);
    require(storage::readBookState(paths, "future").status == ReadStatus::Unsupported &&
            !storage::writeBookState(paths, "future", qt::object({{"book", "future"}})) && bytes(future) == futureBytes,
            "Future book format was changed");
    require(storage::writeJson(paths.preferences(), qt::object({{"bionic_reading", "bold"}, {"smooth_images", true},
            {"last_header_footer", "alternative_b"}, {"inherit_header_footer", true}, {"heavier_interface_text", true},
            {"pin_stats", true}, {"hide_tab_labels", true}, {"columns", 2}, {"column_spacing", 20},
            {"word_spacing", 17}, {"private_optional", "keep"}})), "Settings fixture");
    const auto clean = storage::readPreferences(paths.preferences());
    require(clean.status == ReadStatus::Ok && clean.object.value("schema") == 1 &&
            clean.object.value("bold_reading").toString() == "bold" && clean.object.value("header_footer").toString() == "alternative_b" &&
            clean.object.value("default_menu_view").toString() == "stats" && clean.object.value("hide_tab_labels") == true &&
            clean.object.value("private_optional").toString() == "keep" && !clean.object.contains("word_spacing") &&
            !clean.object.contains("smooth_images") && !clean.object.contains("bionic_reading") &&
            !clean.object.contains("pin_stats") && !clean.object.contains("columns"), "Settings cleanup lost intent");
    require(storage::readJson(paths.preferences()) == clean.object, "Clean settings were not persisted");
    write(paths.preferences(), "{\"schema\":99,\"new_option\":true}");
    const auto settingsBytes = bytes(paths.preferences());
    require(storage::readPreferences(paths.preferences()).status == ReadStatus::Unsupported &&
            !storage::writePreferences(paths.preferences(), {}) && bytes(paths.preferences()) == settingsBytes,
            "Future settings were overwritten");
    QTemporaryDir interrupted;
    const storage::Paths retry{interrupted.path()};
    require(storage::readRecord(retry.directory, "initialize", {}).status == ReadStatus::Missing, "Initialize retry profile");
    require(storage::writeJson(retry.bookState(identity), state), "Interrupted import fixture");
    execute(retry.directory, "CREATE TRIGGER fail_import BEFORE INSERT ON book_state BEGIN SELECT RAISE(ABORT,'test'); END");
    require(storage::readBookState(retry, identity).status == ReadStatus::DatabaseError &&
            QFile::exists(retry.bookState(identity)), "Failed import removed its source");
    execute(retry.directory, "DROP TRIGGER fail_import");
    write(retry.directory + "/migration-backup", "Blocked archive");
    require(storage::readBookState(retry, identity).object == state && QFile::exists(retry.bookState(identity)),
            "Archive failure prevented a committed import");
    require(storage::writeBookState(retry, identity, edited), "Edit after interrupted archive");
    require(QFile::remove(retry.directory + "/migration-backup") &&
            storage::readBookState(retry, identity).object == edited && !QFile::exists(retry.bookState(identity)),
            "Retry imported stale data or failed to finish cleanup");
    write(retry.directory + "/unknown.json", "Do not touch");
    require(storage::prepareProfile(retry) && bytes(retry.directory + "/unknown.json") == "Do not touch",
            "Cleanup changed an unrecognized file");
    require(QDir().mkpath(retry.directory + "/diagnostics"), "Diagnostic directory fixture");
    write(retry.directory + "/diagnostics/events.jsonl", "New session\n");
    write(retry.directory + "/events.jsonl", "Previous session\n");
    require(storage::prepareProfile(retry) && !QFile::exists(retry.directory + "/events.jsonl") &&
            bytes(retry.directory + "/diagnostics/events.jsonl") == "New session\n" &&
            bytes(retry.directory + "/migration-backup/diagnostics/events.jsonl") == "Previous session\n",
            "Startup log prevented cleanup or replaced previous diagnostics");
    QTemporaryDir beta3;
    const storage::Paths oldPaths{beta3.path()};
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
    BookmarkStore oldMarks(oldPaths.annotations(), "native-beta3", {}, "Invented book");
    const Bookmark mark{"/body/p[3]", "First chapter", "An invented passage", QDateTime::currentDateTimeUtc()};
    oldMarks.setBookmarks({mark});
    require(oldMarks.save(), "Beta 3 bookmark fixture");
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
    require(storage::writeJson(oldPaths.bookState("native-beta3"), qt::object({{"schema", 1}, {"book", "native-beta3"},
            {"position", mark.position}, {"font_pixels", 39}})), "Beta 3 reading fixture");
    require(storage::prepareProfile(oldPaths) && !QFile::exists(oldMarks.folder()), "Beta 3 profile cleanup");
    require(AnnotationDatabase::transaction(beta3.path(), [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        return query.exec("PRAGMA user_version") && query.next() && query.value(0).toInt() == 3;
    }), "Beta 3 migration did not create SQLite schema version 3");
    BookmarkStore newMarks(oldPaths.annotations(), "native-beta3", {}, "Invented book", oldPaths.database());
    require(newMarks.load() && newMarks.bookmarks().size() == 1 &&
            newMarks.bookmarks().first().position == mark.position &&
            storage::readBookState(oldPaths, "native-beta3").object.value("font_pixels") == 39 &&
            storage::prepareProfile(oldPaths), "Beta 3 bookmark or settings lost on retry");
    QTemporaryDir editions;
    const storage::Paths editionPaths{editions.path()};
    const QString editedIdentity = "file:///books/changed.epub";
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
    for (const char version : {'a', 'b'}) {
        BookmarkStore old(editionPaths.annotations(), editedIdentity, QByteArray(64, version), "Changed book");
        auto entry = mark;
        entry.excerpt = version == 'a' ? "Newest saved edition" : "Older saved edition";
        old.setBookmarks({entry}); require(old.save(), "Edition fixture");
        // Alphabetical traversal imports the newer source first.
        struct utimbuf stamp { 1600000000, version == 'a' ? 1600000200 : 1600000100 };
        require(::utime(QFile::encodeName(old.folder() + "/bookmarks.json").constData(), &stamp) == 0,
                "Set edition chronology");
    }
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
    require(storage::prepareProfile(editionPaths), "Import saved editions");
    BookmarkStore changed(editionPaths.annotations(), editedIdentity, QByteArray(64, 'c'), "Changed book", editionPaths.database());
    require(changed.load() && changed.bookmarks().size() == 1 && changed.bookmarks().first().excerpt == "Newest saved edition",
            "Bulk migration chose an edition by traversal order instead of source chronology");
    QTemporaryDir unsupportedDatabase;
    execute(unsupportedDatabase.path(), "PRAGMA user_version=1");
    const auto unsupportedBytes = bytes(unsupportedDatabase.path() + "/kobalt.sqlite");
    require(storage::readBookState(storage::Paths{unsupportedDatabase.path()}, "new").status == ReadStatus::DatabaseError &&
            bytes(unsupportedDatabase.path() + "/kobalt.sqlite") == unsupportedBytes,
            "Unsupported SQLite schema was changed");
    QTemporaryDir partialDatabase;
    execute(partialDatabase.path(), "DROP TABLE library_folders");
    const auto partialBytes = bytes(partialDatabase.path() + "/kobalt.sqlite");
    require(storage::readBookState(storage::Paths{partialDatabase.path()}, "new").status == ReadStatus::DatabaseError &&
            bytes(partialDatabase.path() + "/kobalt.sqlite") == partialBytes,
            "Partial schema was repaired or replaced");
    QTemporaryDir futureDatabase;
    execute(futureDatabase.path(), "PRAGMA user_version=4");
    const auto futureDatabaseBytes = bytes(futureDatabase.path() + "/kobalt.sqlite");
    require(storage::readBookState(storage::Paths{futureDatabase.path()}, "new").status == ReadStatus::DatabaseError &&
            bytes(futureDatabase.path() + "/kobalt.sqlite") == futureDatabaseBytes,
            "Future SQLite schema was changed");


#ifndef KOBALT_TEST_NO_LIBRARY
    QTemporaryDir catalogue;
    library::Index index;
    index.addFolder({"/invented", "Invented books"});
    library::Book book;
    book.key = "/invented/river.epub"; book.folder = "/invented"; book.name = "river.epub";
    book.title = "The Copper River"; book.author = "Robin Example"; book.seriesIndex = 2.5;
    book.opened = 1792849147000; book.percent = 43; book.mark = library::Book::Mark::Finished;
    index.books.insert(book.key, book);
    index.settings.sort = library::Sort::Author; index.settings.hideFinished = true;
    require(index.save(catalogue.path() + "/library.json"), "Legacy catalogue fixture");
    library::Index imported;
    require(imported.loadDatabase(catalogue.path()) && imported.books.value(book.key).json() == book.json() &&
            imported.settings.sort == library::Sort::Author && imported.settings.hideFinished &&
            !QFile::exists(catalogue.path() + "/library.json"), "Catalogue import changed books or preferences");
    imported.books[book.key].percent = 81;
    require(imported.saveDatabase(catalogue.path()), "Save catalogue update");
    library::Index reopened;
    require(reopened.loadDatabase(catalogue.path()) && reopened.books.value(book.key).percent == 81,
            "Catalogue update lost");
    execute(catalogue.path(), "CREATE TRIGGER unchanged_book BEFORE INSERT ON library_books BEGIN SELECT RAISE(ABORT,'duplicate'); END");
    require(reopened.saveDatabase(catalogue.path()), "Unchanged catalogue rewrote every book");
    execute(catalogue.path(), "DROP TRIGGER unchanged_book");
    reopened.books.clear(); require(reopened.saveDatabase(catalogue.path()), "Remove catalogue book");
    require(imported.loadDatabase(catalogue.path()) && imported.books.isEmpty(), "Removed book reimported");
    execute(catalogue.path(), "PRAGMA user_version=99");
    const auto databaseBytes = bytes(catalogue.path() + "/kobalt.sqlite");
    require(!imported.loadDatabase(catalogue.path()) &&
            bytes(catalogue.path() + "/kobalt.sqlite") == databaseBytes, "Future database was modified");

#endif
    return 0;
}
