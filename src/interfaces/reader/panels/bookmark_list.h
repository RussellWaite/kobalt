#pragma once
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/rows.h"
#include "interfaces/reader/ui/controls/paging.h"
#include "interfaces/reader/ui/controls/panels.h"
#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QStringList>
#include "interfaces/reader/ui/dialogs/modals.h"
#include <functional>

namespace kobalt {
struct BookmarkRow {
    QString position, chapter, excerpt;
    QDateTime created;
    int page = -1; // Zero-based, or -1 when the position no longer resolves.
    bool current = false;
    int endPage = -1;
    QString style = {};
    QString color = QStringLiteral("gray");
    int sourceOrder = -1;
};

// The Bookmarks list covers the header's action row with its own bar, like
// the Chapters list. Rows go to their page; each row's remove cell asks first.
class BookmarkList : public ui::Panel {
public:
    explicit BookmarkList(QWidget *parent);
    void showBookmarks(QVector<BookmarkRow> rows);
    void showAnnotations(QVector<BookmarkRow> rows, bool loading = false);
    void setSortOrder(const QString &order);
    QString sortOrder() const { return order; }
    std::function<void(const QString &)> sortRequested;
    void setExcerptFont(const QString &family);
    bool annotationsShown() const {
        return annotations;
    }
    std::function<void(bool)> tabRequested;
    std::function<void(const QString &)> selected, removeRequested;
    std::function<void()> back;
    std::function<void()> actionsRequested;
    std::function<void()> loadingCancelled;
    // Return chapter/excerpt rows in the requested ID order for one visible page.
    std::function<bool(const QStringList &, QVector<BookmarkRow> *)> annotationDetails;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void showItems(QVector<BookmarkRow> list, bool annotationTab, bool pending = false);
    void sortItems();
    void arrange();
    void redraw();
    int pageCount() const;
    QVector<BookmarkRow> items;
    QStringList visibleDetailIds;
    QVector<BookmarkRow> visibleDetails;
    bool visibleDetailsReady = false;
    QVector<QPushButton *> buttons, removes;
    QPushButton *bookmarksTab, *annotationsTab;
    ui::CellButton *sortButton;
    ui::CellButton *moreButton;
    QPointer<ui::ModalOverlay> sortPopup;
    void openSort();
    void closeSort();
    QVector<int> pageStarts, itemHeights, excerptLines;
    QHash<QString, int> excerptLineCache;
    QFont excerptCacheFont;
    int excerptCacheWidth = -1, excerptCacheLineHeight = -1;
    QString order = QStringLiteral("position");
    bool annotations = false;
    bool loading = false;
    QString excerptFamily;
    int listTop() const;
    int rowHeight() const;
    QFont excerptFont() const;
    QLabel *summary, *pageHeading, *empty, *emptyHint;
    ui::ListHeader *listHeader;
    ui::CellButton *close;
    ui::PagerBar *pager;
    int page = 0, rows = 1;
};
}
