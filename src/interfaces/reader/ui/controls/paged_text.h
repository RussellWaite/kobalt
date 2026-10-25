#pragma once
#include "../style/rich_text.h"
#include <QTextDocument>
#include <QTextBlock>
#include <QMargins>
#include <QWidget>

namespace kobalt::ui {
// Qt lays out rich text without a web view. Resources cannot start I/O while
// a definition is visible or while Nickel is handing storage to USB.
class PagedDocument : public QTextDocument {
protected:
    QVariant loadResource(int, const QUrl &) override {
        return {};
    }
};
class PagedText : public QWidget {
public:
    explicit PagedText(QWidget *parent = nullptr);
    void setHtml(const QString &html);
    void setTextFont(const QFont &font);
    // Minimum leading relative to the font. Zero keeps the source formatting.
    void setMinimumLineHeight(int percent);
    QFont textFont() const {
        return document.font();
    }
    void setMargins(const QMargins &value);
    int pages() const;
    int page() const {
        return current;
    }
    void setPage(int number);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void changeEvent(QEvent *event) override;

private:
    PrivateRichText document;
    int current = 0;
    int minimumLineHeight = 0;
    QMargins margins;
    QString source;
    void paginate();
};
}
