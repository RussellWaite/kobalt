#pragma once
#include "storage/highlight_store.h"

#include <QObject>
#include <QRect>
#include <atomic>
#include <functional>
#include <memory>

namespace kobalt {
class ReaderSession;
struct BookmarkLocations;
class HighlightSession final : public QObject {
public:
    HighlightSession(ReaderSession &model, std::function<QString()> layout, const QString &annotations,
                     const QString &identity, const QByteArray &fingerprint, const QString &title, const QString &database = {});
    ~HighlightSession() override;
    bool available() const {
        return bool(store_);
    }
    bool writable() const {
        return store_ && !loadFailed_;
    }
    QString error() const {
        return error_;
    }
    const QVector<Highlight> &highlights() const;
    // Ordinary reading fetches payloads only for visible rows or an edited passage.
    bool excerpts(const QStringList &ids, QVector<HighlightExcerpt> *rows);
    bool detail(const QString &id, Highlight *row);
    // Fetch one revision-checked snapshot in source order. Requires resolved locations.
    bool exportDetails(QVector<Highlight> *rows);
    bool sync();
    // Stop an abandoned lookup without discarding already resolved locations.
    void cancelLookup();
    bool resolved() const;
    // One validity check for callers rendering an entire annotation list.
    BookmarkLocations locations() const;
    int page(int index) const;
    int endPage(int index) const;
    int sourceOrder(int index) const { return resolved() ? order_.value(index * 2, -1) : -1; }
    // Empty id creates a distinct passage. Existing IDs replace that passage.
    // Ranges may touch, but cannot overlap another saved passage.
    // Completion is always called while this session lives, false on stale work.
    void save(const QString &start, const QString &end, const QString &id = {}, std::function<void(bool)> finished = {},
              const QString &chosenEnd = {}, bool sentenceEndPolicy = false, const QString &chosenStart = {},
              bool sentenceStartPolicy = false, const QString &color = {}, const QString &style = {});
    // Omitted save colors preserve an existing passage, or use gray for a new one.
    // Recolor changes only the color and commits atomically with rollback on failure.
    bool recolor(const QString &id, const QString &color);
    bool restyle(const QString &id, const QString &style);
    // Empty fields preserve saved values. Both changes persist together or roll back.
    bool updateAppearance(const QString &id, const QString &color, const QString &style);
    bool remove(const QString &id);
    std::function<void()> changed, failed;

private:
    bool persist(QVector<Highlight> highlights);
    ReaderSession &model_;
    std::function<QString()> layout;
    std::unique_ptr<HighlightStore> store_;
    QString layout_, lookup_, error_;
    QVector<int> pages_, order_;
    std::shared_ptr<std::atomic_bool> lookupCancelled_;
    quint64 generation_ = 0;
    quint64 layoutRevision_ = 0, lookupRevision_ = 0, lookupRequest_ = 0;
    quint64 rangesGeneration_ = 0, lookupGeneration_ = 0;
    bool loadFailed_ = false;
};
}
