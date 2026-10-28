#include "qt_compat.h"
#include "diagnostics.h"
#include "strings.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/resource.h>
#endif

namespace kobalt {
namespace {
QMutex mutex;
QString logPath;
bool paused = false;
bool syncing = true;
std::function<void(const QJsonObject &)> observer;
constexpr qint64 limit = 128 * 1024;
// Resident and peak resident memory in KiB, or -1 where unknown.
void addMemory(QJsonObject &fields) {
#ifdef __APPLE__
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        fields["rss_kib"] = double(info.resident_size / 1024);
    rusage usage{};
    // The Mac reports the peak in bytes, Linux in KiB.
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        fields["peak_rss_kib"] = double(usage.ru_maxrss / 1024);
#else
    QFile memory("/proc/self/status");
    if (memory.open(QIODevice::ReadOnly)) {
        for (const auto &line : memory.readAll().split('\n')) {
            if (line.startsWith("VmRSS:") || line.startsWith("VmHWM:")) {
                const auto parts = line.simplified().split(' ');
                if (parts.size() >= 2)
                    fields[line.startsWith("VmRSS:") ? "rss_kib" : "peak_rss_kib"] = parts[1].toDouble();
            }
        }
    }
#endif
}
}
void setDiagnosticsSync(bool enabled) {
    QMutexLocker lock(&mutex);
    syncing = enabled;
}
void setDiagnosticObserver(std::function<void(const QJsonObject &)> value) {
    QMutexLocker lock(&mutex);
    observer = std::move(value);
}
QJsonObject diagnosticDeviceFields(const QByteArray &version) {
    const auto fields = version.trimmed().split(',');
    if (fields.size() != 6) {
        return {};
    }
    QJsonObject result;
    const auto firmware = QString::fromLatin1(fields[2].trimmed());
    const auto hardware = QString::fromLatin1(fields[5].trimmed());
    // Persist only model and firmware fields. The first field is the serial.
    static const QRegularExpression firmwarePattern(QStringLiteral("^[0-9]+(?:\\.[0-9]+){2,}$"));
    static const QRegularExpression hardwarePattern(QStringLiteral("^00000000-0000-0000-0000-[0-9]{12}$"));
    if (firmwarePattern.match(firmware).hasMatch()) {
        result["firmware"] = firmware;
    }
    if (hardwarePattern.match(hardware).hasMatch()) {
        result["hardware_uuid"] = hardware;
    }
    return result;
}
QString diagnosticErrorCode(const QString &reason) {
    if (reason.isEmpty())
        return {};
    int count = 0;
    const auto *entries = strings::entries(&count);
    // Never persist arbitrary error text: it may contain a private file path.
    for (int i = 0; i < count; ++i) {
        const auto &entry = entries[i];
        if (std::strncmp(entry.key, "error.", 6) == 0 &&
            (reason == QString::fromUtf8(entry.value) || reason == QLatin1String(entry.key)))
            return QString::fromLatin1(entry.key);
    }
    return QStringLiteral("unknown");
}
void pauseDiagnostics(bool value) {
    QMutexLocker lock(&mutex);
    paused = value;
}
bool diagnosticsActive() {
    QMutexLocker lock(&mutex);
    return !paused && !logPath.isEmpty();
}
bool diagnosticsRequested(const QString &profile) {
    return qgetenv("KOBALT_DIAGNOSTICS") == "1" || QFile::exists(profile + "/diagnostics-enabled");
}
void initializeDiagnostics(const QString &directory) {
    {
        QMutexLocker lock(&mutex);
        if (!QDir().mkpath(directory))
            return;
        logPath = directory + "/events.jsonl";
    }
    auto fields = kobalt::qt::object({{"build", __DATE__ " " __TIME__}, {"qt", qVersion()}});
#ifdef KOBALT_VERSION
    fields["version"] = QString::fromLatin1(KOBALT_VERSION);
#endif
#ifdef KOBALT_READER_HARDWARE
    QFile version("/mnt/onboard/.kobo/version");
    if (version.open(QIODevice::ReadOnly)) {
        const auto device = diagnosticDeviceFields(version.read(512));
        for (auto it = device.begin(); it != device.end(); ++it) {
            fields[it.key()] = it.value();
        }
    }
#endif
    diagnostic("startup", fields);
}
void diagnostic(const char *event, QJsonObject fields, bool durable) {
    const int savedErrno = errno;
    QMutexLocker lock(&mutex);
    if (paused || logPath.isEmpty())
        return;
    struct timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    fields["event"] = QString::fromLatin1(event);
#if QT_VERSION >= QT_VERSION_CHECK(5, 8, 0)
    fields["utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
#else
    fields["utc"] = QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm:ss.zzz") + 'Z';
#endif
    fields["monotonic_ms"] = double(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
    fields["pid"] = int(getpid());
    addMemory(fields);
    const auto bytes = QJsonDocument(fields).toJson(QJsonDocument::Compact) + '\n';
    // Keep Nickel's crash handlers. Durable boundaries leave the last operation
    // on storage without adding a signal handler that could itself fail.
    if (bytes.size() <= 4096) {
        const auto path = QFile::encodeName(logPath);
        int fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        struct stat info {};
        if (fd >= 0 && fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
            if (info.st_size + bytes.size() > limit) {
                ::close(fd);
                fd = -1;
                const auto previous = path + ".previous";
                if (::rename(path.constData(), previous.constData()) == 0)
                    fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            }
            if (fd >= 0) {
                qint64 written = 0;
                while (written < bytes.size()) {
                    const auto count = ::write(fd, bytes.constData() + written, bytes.size() - written);
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count <= 0)
                        break;
                    written += count;
                }
                if (durable && syncing)
                    ::fdatasync(fd);
            }
        }
        if (fd >= 0)
            ::close(fd);
    }
    // Call outside the lock, so the observer may write events itself.
    const auto notify = observer;
    lock.unlock();
    if (notify)
        notify(fields);
    errno = savedErrno;
}
}
