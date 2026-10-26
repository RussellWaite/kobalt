#pragma once
#include <functional>
#include <mutex>
#include <pthread.h>
#include <thread>

namespace kobalt::storage {
// Runs profile writes on its own thread, so reading never waits for a commit
// and its storage syncs. Only the newest write waits: posting replaces a write
// that has not started. Each write owns its database connection, as on the
// GUI thread, so nothing stays open between writes.
class RecordWriter {
public:
    using Write = std::function<bool()>;
    using Done = std::function<void(bool)>;
    RecordWriter() = default;
    RecordWriter(const RecordWriter &) = delete;
    RecordWriter &operator=(const RecordWriter &) = delete;
    // Finishes the running and pending write before the thread ends.
    ~RecordWriter();
    // `done` runs on the writer thread with the result of `write`.
    void post(Write write, Done done);
    // Drops a pending write and waits for a running one. A synchronous write
    // that follows cannot then be overwritten by an older one.
    void cancel();
    // Waits for the pending and running write. False when `milliseconds`
    // pass first; a negative value waits without a limit.
    bool flush(int milliseconds = -1);
    bool idle() const;

private:
    void run();
    mutable std::mutex mutex;
    // The Qt 6 toolchain links std::condition_variable::wait against
    // GLIBCXX_3.4.30, which firmware 6 does not provide, so Nickel could not
    // load the library. Wait on pthread conditions with the std::mutex's
    // handle, as engine_activity.h does.
    pthread_cond_t wake = PTHREAD_COND_INITIALIZER, finished = PTHREAD_COND_INITIALIZER;
    Write pending;
    Done pendingDone;
    bool running = false, stopping = false;
    std::thread thread;
};
}
