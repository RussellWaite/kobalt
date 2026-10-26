#pragma once
#include <QJsonObject>
#include <QDateTime>
#include <QElapsedTimer>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>
#include <functional>

class QIODevice;
class QNetworkReply;

namespace kobalt::kosync {
// KOReader hashes 1 KiB samples at exponentially spaced offsets, not the whole archive.
QByteArray documentDigest(QIODevice &file);
QByteArray filenameDigest(const QString &path);
QUrl serverUrl(const QString &text);
struct Progress {
    QString position, device, deviceId;
    double percentage = 0;
    qint64 timestamp = 0;
    bool found = false;
};
bool parseProgress(const QJsonObject &object, Progress *progress);

// A failed fetch never reaches this reconciler. A remote choice must be
// acknowledged before any local position can be uploaded.
class Reconciler {
public:
    enum class Action { Current, Upload, Choose };
    Action observe(const Progress &remote, const QString &position, const QString &deviceId);
    bool pending() const { return pending_.found; }
    bool needsSync(const QString &position) const { return !position.isEmpty() && position != uploaded_; }
    Progress candidate() const { return pending_; }
    void keepLocal() { pending_ = {}; uploaded_.clear(); }
    void acceptRemote(const QString &position) { pending_ = {}; uploaded_ = position; }
    void uploaded(const QString &position) { uploaded_ = position; }
private:
    Progress observed_, pending_;
    QString uploaded_;
};

// One reader owns one client. Requests are bounded, cancellable and never redirect credentials.
class Client : public QObject {
public:
    explicit Client(QString accountPath, QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    ~Client() override;
    bool signedIn() const { return !key_.isEmpty(); }
    bool busy() const { return !reply_.isNull() || retryTimer_.isActive(); }
    bool automatic() const { return automatic_; }
    QString server() const { return server_.toString(); }
    QString username() const { return username_; }
    QString deviceId() const { return deviceId_; }
    QString deviceName() const { return deviceName_.isEmpty() ? defaultDeviceName_ : deviceName_; }
    QString customDeviceName() const { return deviceName_; }
    QString defaultDeviceName() const { return defaultDeviceName_; }
    void setDefaultDeviceName(const QString &name);
    bool setDeviceName(const QString &name);
    bool filenameMatching() const { return filenameMatching_; }
    bool setFilenameMatching(bool enabled);
    QString storageError() const { return storageError_; }
    using Done = std::function<void(QString error)>;
    void login(const QString &server, const QString &username, const QString &password, Done done);
    bool setAutomatic(bool enabled);
    bool logout();
    QDateTime lastSync() const { return lastSync_; }
    bool recordSyncSuccess();
    void cancel();
    void pull(const QByteArray &document, std::function<void(Progress, QString)> done, bool retryTransient = true);
    void push(const QByteArray &document, const Progress &progress, Done done);

private:
    using Response = std::function<void(int, QJsonObject, QString)>;
    void request(const QByteArray &method, const QString &path, const QUrl &server,
                 const QString &username, const QByteArray &key, const QJsonObject &body, Response done, bool retryTransient = true);
    void requestAttempt(const QByteArray &method, const QString &path, const QUrl &server,
                        const QString &username, const QByteArray &key, const QJsonObject &body, Response done, int attempt);
    QTimer retryTimer_;
    QElapsedTimer requestElapsed_;
    bool save(const QUrl &server, const QString &username, const QByteArray &key, bool automatic,
              const QDateTime &lastSync, const QString &deviceName, bool filenameMatching);
    QString accountPath_, username_, deviceId_, storageError_;
    QString deviceName_, defaultDeviceName_ = QStringLiteral("Kobalt");
    QUrl server_;
    QDateTime lastSync_;
    QByteArray key_;
    bool automatic_ = true, writable_ = true;
    bool filenameMatching_ = false;
    QNetworkAccessManager ownedNetwork_;
    QNetworkAccessManager *network_;
    QPointer<QNetworkReply> reply_;
};
}
