#include "record_writer.h"
#include <cerrno>
#include <time.h>
#include <utility>

namespace kobalt::storage {
namespace {
template <class Ready> void wait(pthread_cond_t &condition, std::unique_lock<std::mutex> &lock, Ready ready) {
    while (!ready())
        pthread_cond_wait(&condition, lock.mutex()->native_handle());
}
// False when `milliseconds` pass first.
template <class Ready>
bool waitFor(pthread_cond_t &condition, std::unique_lock<std::mutex> &lock, int milliseconds, Ready ready) {
    timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += long(milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    while (!ready())
        if (pthread_cond_timedwait(&condition, lock.mutex()->native_handle(), &deadline) == ETIMEDOUT)
            return ready();
    return true;
}
}
RecordWriter::~RecordWriter() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    pthread_cond_broadcast(&wake);
    if (thread.joinable())
        thread.join();
    pthread_cond_destroy(&wake);
    pthread_cond_destroy(&finished);
}
void RecordWriter::post(Write write, Done done) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending = std::move(write);
        pendingDone = std::move(done);
        if (!thread.joinable())
            thread = std::thread([this] { run(); });
    }
    pthread_cond_broadcast(&wake);
}
void RecordWriter::cancel() {
    std::unique_lock<std::mutex> lock(mutex);
    pending = {};
    pendingDone = {};
    wait(finished, lock, [this] { return !running; });
}
bool RecordWriter::flush(int milliseconds) {
    std::unique_lock<std::mutex> lock(mutex);
    const auto drained = [this] { return !running && !pending; };
    if (milliseconds < 0) {
        wait(finished, lock, drained);
        return true;
    }
    return waitFor(finished, lock, milliseconds, drained);
}
bool RecordWriter::idle() const {
    std::lock_guard<std::mutex> lock(mutex);
    return !running && !pending;
}
void RecordWriter::run() {
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
        wait(wake, lock, [this] { return stopping || pending; });
        if (!pending)
            return;
        auto write = std::move(pending);
        auto done = std::move(pendingDone);
        pending = {};
        pendingDone = {};
        running = true;
        lock.unlock();
        const bool saved = write();
        if (done)
            done(saved);
        lock.lock();
        running = false;
        pthread_cond_broadcast(&finished);
    }
}
}
