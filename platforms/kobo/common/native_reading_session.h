#pragma once
#include <QString>
#include <memory>
#include "reading_estimates.h"
#include "reading_pace_cache.h"

namespace kobalt {
// Construct only after firmwareSupported() accepts Nickel, on the GUI thread.
class NativeReadingSession {
public:
    explicit NativeReadingSession(QString book);
    ~NativeReadingSession();
    static bool available();
    void start(const ReadingActivity &activity);
    void page(const ReadingActivity &activity);
    bool estimates(const ReadingActivity &activity, ReadingEstimates *result);
    bool estimates(ReadingEstimates *result) {
        return estimates(activity_.current(), result);
    }
    void stop();
    std::function<void()> estimatesChanged;
#ifndef KOBALT_READER_HARDWARE
    static void testCommand();
#endif
private:
    // Fires the page event once the event worker has checked the account.
    void report(bool signedIn, int words);
    QString book, account;
    double percent = 0;
    bool running = false;
    // Page reports check the account on Nickel's worker. A late result for
    // an earlier session or a destroyed one is ignored.
    int epoch = 0;
    std::shared_ptr<NativeReadingSession *> alive = std::make_shared<NativeReadingSession *>(this);
    nickel::PageReadingActivity activity_;
    nickel::ReadingPaceCache pace_;
};
}
