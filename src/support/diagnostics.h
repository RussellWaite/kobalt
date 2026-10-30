#pragma once
#include "qt_compat.h"
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <functional>

namespace kobalt {
QJsonObject diagnosticDeviceFields(const QByteArray &version);
QString diagnosticErrorCode(const QString &reason);
void pauseDiagnostics(bool paused);
// True when events are being written, so callers can skip work that only feeds diagnostics.
bool diagnosticsActive();
void initializeDiagnostics(const QString &directory);
bool diagnosticsRequested(const QString &profile);
// Queues an event for the log. The file is written on the log's own thread.
void diagnostic(const char *event, QJsonObject fields = {}, bool durable = true);
// Waits until queued events are in the log file. False when `milliseconds`
// pass first; a negative value waits without a limit. For tests and tools
// that read the file; the reader never needs to wait for its own log.
bool flushDiagnostics(int milliseconds = -1);
// Whether durable events sync the log to storage. Performance scenarios turn
// this off, so their timings leave out those syncs.
void setDiagnosticsSync(bool enabled);
// Receives every event that is written, on the thread that wrote it. An empty
// function removes the observer.
void setDiagnosticObserver(std::function<void(const QJsonObject &)> observer);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
// Qt 5.2 cannot build a QJsonObject from a braced list of fields.
inline void diagnostic(const char *event, std::initializer_list<qt::JsonField> fields, bool durable = true) {
    diagnostic(event, qt::object(fields), durable);
}
#endif
}
