#include "theme.h"
#include "metrics.h"
#include "fonts.h"
#include <QPainter>
#include <QVariant>

namespace kobalt::ui {
namespace m = metrics;
const ThemeColors &themeColors(bool dark) {
    static const ThemeColors light = [] {
        ThemeColors colors;
        colors.paper = QColor(Qt::white);
        colors.ink = QColor(Qt::black);
        colors.secondaryInk = QColor(0x333333);
        colors.disabledInk = QColor(0x999999);
        colors.divider = QColor(0xb8b8b8);
        colors.outline = QColor(0x666666);
        colors.pressedPaper = QColor(0xdddddd);
        colors.swipeCaptionInk = QColor(0x666666);
        colors.lightHintPaper = QColor(0xe6e6e6);
        colors.sleepPaper = QColor(Qt::white);
        colors.overviewPaper = QColor(0xf1f1f1);
        colors.openingTrack = QColor(0xd3d3d3);
        colors.modalFade = colors.paper;
        colors.modalFade.setAlpha(90);
        colors.modalPattern = QColor(85, 85, 85, 150);
        colors.sleepDim = QColor(0, 0, 0, 40);
        return colors;
    }();
    static const ThemeColors night = [] {
        ThemeColors colors;
        colors.paper = QColor(Qt::black);
        colors.ink = QColor(Qt::white);
        colors.secondaryInk = QColor(0xcccccc);
        colors.disabledInk = QColor(0x999999);
        colors.divider = QColor(0x666666);
        colors.outline = QColor(0xbbbbbb);
        colors.pressedPaper = QColor(0x333333);
        colors.swipeCaptionInk = QColor(0xaaaaaa);
        colors.lightHintPaper = QColor(0x222222);
        colors.sleepPaper = QColor(0x202020);
        colors.overviewPaper = QColor(0x242424);
        colors.openingTrack = QColor(0x2c2c2c);
        colors.modalFade = colors.paper;
        colors.modalFade.setAlpha(90);
        colors.modalPattern = QColor(170, 170, 170, 150);
        colors.sleepDim = QColor(0, 0, 0, 40);
        return colors;
    }();
    return dark ? night : light;
}
const ThemeColors &themeColors(const QWidget *widget) {
    return themeColors(darkTheme(widget));
}
namespace {
QRectF pixelAlignedRect(const QPainter &painter, const QRectF &rect) {
    const auto transform = painter.deviceTransform();
    if (transform.type() > QTransform::TxScale || transform.m11() <= 0 || transform.m22() <= 0)
        return rect;
    const QRectF device = transform.mapRect(rect);
    const QRectF aligned(qRound(device.left()), qRound(device.top()),
                         qMax(1, qRound(device.right()) - qRound(device.left())),
                         qMax(1, qRound(device.bottom()) - qRound(device.top())));
    return transform.inverted().mapRect(aligned);
}
}
void paintEdges(QPainter &painter, const QRect &rect, Edges edges, const QColor &ink, int width) {
    if (rect.isEmpty() || width <= 0 || !edges)
        return;
    const QRectF bounds(rect);
    // Snap both boundaries of each strip. Rounding its width separately can
    // extend the rule under adjacent opaque widgets at fractional scales.
    const auto fill = [&](const QRectF &strip) {
        painter.fillRect(pixelAlignedRect(painter, strip), ink);
    };
    if (edges.testFlag(Qt::TopEdge))
        fill(QRectF(bounds.left(), bounds.top(), bounds.width(), width));
    if (edges.testFlag(Qt::BottomEdge))
        fill(QRectF(bounds.left(), bounds.bottom() - width, bounds.width(), width));
    if (edges.testFlag(Qt::LeftEdge))
        fill(QRectF(bounds.left(), bounds.top(), width, bounds.height()));
    if (edges.testFlag(Qt::RightEdge))
        fill(QRectF(bounds.right() - width, bounds.top(), width, bounds.height()));
}
void paintButtonShape(QPainter &painter, const QRect &rect, const QColor &paper, const QColor &fill,
                      const QColor &border) {
    painter.fillRect(rect, paper);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(border, m::control::border));
    painter.setBrush(fill);
    const qreal inset = int(m::control::border) / 2.0;
    painter.drawRoundedRect(pixelAlignedRect(painter, rect).adjusted(inset, inset, -inset, -inset),
                            m::control::buttonRadius, m::control::buttonRadius);
    painter.restore();
}
bool darkTheme(const QWidget *widget) {
    for (auto *ancestor = widget; ancestor; ancestor = ancestor->parentWidget()) {
        const auto theme = ancestor->property("reader_dark");
        if (theme.isValid())
            return theme.toBool();
    }
    return false;
}
QColor dividerColor(const QWidget *widget) {
    return themeColors(widget).divider;
}
QColor sectionColor(const QWidget *widget) {
    return themeColors(widget).pressedPaper;
}
QColor noteColor(const QWidget *widget) {
    return themeColors(widget).secondaryInk;
}
void paintHeaderRules(QPainter &painter, const QWidget *view) {
    painter.fillRect(0, m::chrome::statusHeight - m::control::border, view->width(), m::control::border,
                     view->palette().color(QPalette::Mid));
    painter.fillRect(0, m::chrome::headerHeight - m::chrome::heavyRule, view->width(), m::chrome::heavyRule,
                     view->palette().color(QPalette::WindowText));
}
void readerStyle(QWidget *widget, const QString &source) {
    widget->setProperty("reader_style", source);
    const auto &colors = themeColors(widget);
    auto style = QString("QPushButton{border-radius:%1px;} ").arg(int(m::control::buttonRadius));
    style += source.contains('{') ? source : QString("*{%1}").arg(source);
    const auto escapedFamily = [](QString name) {
        name.replace('\\', "\\\\");
        name.replace('\"', "\\\"");
        name.replace('\'', "\\'");
        return name;
    };
    style.replace("@ui-font-family@", escapedFamily(interfaceFont().family()));
    style.replace("@ui-caption-family@", escapedFamily(chromeFont().family()));
    style.replace("@header-footer-font@", "\"" + escapedFamily(headerFooterFont().family()) + "\"");
    style += " QPushButton:disabled,QPushButton:checked:disabled,QPushButton:pressed:disabled{"
             "color:palette(mid);background:palette(window);border-color:palette(midlight);}";
    style.replace("palette(window-text)", colors.ink.name());
    style.replace("palette(window)", colors.paper.name());
    style.replace("palette(midlight)", colors.pressedPaper.name());
    style.replace("palette(mid)", colors.disabledInk.name());
    style.replace("palette(light)", colors.divider.name());
    style.replace("palette(placeholder-text)", colors.secondaryInk.name());
    style.replace("palette(dark)", colors.outline.name());
    style.replace("@swipe-caption-ink@", colors.swipeCaptionInk.name());
    if (widget->styleSheet() != style)
        widget->setStyleSheet(style);
}
QPalette readerPalette(QPalette colors, bool dark) {
    const auto &theme = themeColors(dark);
    for (const auto role : {QPalette::Window, QPalette::Base, QPalette::Button, QPalette::HighlightedText})
        colors.setColor(role, theme.paper);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::Highlight})
        colors.setColor(role, theme.ink);
    colors.setColor(QPalette::Mid, theme.disabledInk);
    colors.setColor(QPalette::Midlight, theme.pressedPaper);
    colors.setColor(QPalette::Light, theme.divider);
    colors.setColor(QPalette::Dark, theme.outline);
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
    colors.setColor(QPalette::PlaceholderText, theme.secondaryInk);
#endif
    return colors;
}
void applyReaderPalette(QWidget *widget, const QPalette &colors) {
    widget->setPalette(colors);
    if (widget->property("reader_style").isValid())
        readerStyle(widget, widget->property("reader_style").toString());
    auto styled = widget->palette();
    for (const auto role : {QPalette::Mid, QPalette::Midlight, QPalette::Light, QPalette::Dark,
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
                           QPalette::PlaceholderText,
#endif
         })
        for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
            styled.setColor(group, role, colors.color(group, role));
    widget->setPalette(styled);
}
}
