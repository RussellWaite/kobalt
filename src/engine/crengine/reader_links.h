#pragma once
#include <QString>
#include <QRect>
#include <QVector>

namespace kobalt {
enum class LinkKind { None, Passage, Footnote, Website, Unavailable };
struct PageLink {
    QString source;
    QVector<QRect> rectangles;
    LinkKind kind = LinkKind::None;

    bool tappable(const QString &mode) const {
        return mode == "all" || (mode == "footnotes" && kind == LinkKind::Footnote);
    }
};
struct LinkDetails {
    LinkKind kind = LinkKind::None;
    QString label, target, html, error;
    // The chapter that contains the target, for passages and notes.
    QString chapter;
};
}
