#include "../style/fonts.h"
#include "paged_text.h"
#include "../style/metrics.h"
#include "../style/theme.h"
#include <QPainter>
#include <QEvent>

namespace kobalt::ui {
PagedText::PagedText(QWidget *parent) : QWidget(parent) {
    const int margin = metrics::acknowledgments::textMargin;
    margins = QMargins(margin, margin, margin, margin);
    setAttribute(Qt::WA_OpaquePaintEvent);
    QFont font = kobalt::ui::interfaceFont();
    font.setPixelSize(metrics::text::body);
    document.setFont(font);
}
void PagedText::setHtml(const QString &html) {
    source = html;
    document.setHtml(html);
    current = 0;
    paginate();
    update();
}
void PagedText::setTextFont(const QFont &font) {
    document.setFont(font);
    paginate();
    current = qBound(0, current, pages() - 1);
    update();
}
void PagedText::setMinimumLineHeight(int percent) {
    if (minimumLineHeight == percent)
        return;
    minimumLineHeight = percent;
    document.setHtml(source);
    paginate();
    current = qBound(0, current, pages() - 1);
    update();
}
void PagedText::changeEvent(QEvent *event) {
    // Rebuild inherited font attributes after interface settings change.
    if (event->type() == QEvent::FontChange) {
        const int page = current;
        document.setFont(renewedFont(document.font()));
        document.setHtml(source);
        paginate();
        current = qBound(0, page, pages() - 1);
        update();
    }
    QWidget::changeEvent(event);
}
void PagedText::setMargins(const QMargins &value) {
    margins = value;
    paginate();
    current = qBound(0, current, pages() - 1);
    update();
}
int PagedText::pages() const {
    return document.pages();
}
void PagedText::setPage(int number) {
    current = qBound(0, number, pages() - 1);
    update();
}
void PagedText::resizeEvent(QResizeEvent *) {
    paginate();
    current = qBound(0, current, pages() - 1);
}
void PagedText::paginate() {
    document.layout(qMax(1, width() - margins.left() - margins.right()),
                    qMax(1, height() - margins.top() - margins.bottom()), minimumLineHeight);
}
void PagedText::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    const int pageHeight = qMax(1, height() - margins.top() - margins.bottom());
    const int start = current * pageHeight;
    painter.translate(margins.left(), margins.top() - start);
    document.draw(painter, QRectF(0, start, qMax(1, width() - margins.left() - margins.right()), pageHeight), palette());
}
}
