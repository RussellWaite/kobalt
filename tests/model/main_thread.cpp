#include "support/main_thread.h"
#include "storage/record_writer.h"
#include <QCoreApplication>
#include <QStringList>
#include <cstdio>
#include <thread>
#include "check.h"

using namespace kobalt;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QStringList violations;
    mainthread::setViolationHandler([&](const char *operation) { violations.append(QString::fromLatin1(operation)); });
    storage::RecordWriter writer;
    const auto write = [&] { return writer.writeNow([] { return true; }); };

    require(write() && violations.isEmpty(), "A main-thread write without an interactive screen was reported");
    {
        bool reading = false;
        mainthread::Interaction screen([&] { return reading; });
        require(write() && violations.isEmpty(), "A screen that takes no input reported a write");
        reading = true;
        require(mainthread::interactive(), "A screen that takes input was not interactive");
        require(write() && violations == QStringList{"record-writer-write"},
                "A main-thread write during input was not reported");
        violations.clear();
        {
            mainthread::AllowBlocking allow("the test waits deliberately");
            require(!mainthread::interactive() && write() && violations.isEmpty(),
                    "A deliberate wait was reported");
        }
        require(mainthread::interactive(), "Closing a deliberate wait did not restore the check");
        bool written = false;
        std::thread worker([&] { written = write(); });
        worker.join();
        require(written && violations.isEmpty(), "A write on another thread was reported");
        storage::RecordWriter background;
        background.post([] { return true; }, {});
        require(violations.isEmpty(), "Posting a background write was reported");
        background.flush(0);
        require(violations.isEmpty(), "Polling a background write was reported");
        mainthread::AllowBlocking allow("the test drains its writer");
        background.flush();
    }
    require(!mainthread::interactive() && write() && violations.isEmpty(),
            "A destroyed screen still made writes report");
    mainthread::setViolationHandler({});
    std::puts("PASS: main-thread blocking reports, deliberate waits, other threads and screen lifetime");
}
