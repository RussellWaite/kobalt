#pragma once

#include <QFont>
#include <QColor>
#include <QImage>
#include <QLabel>
#include <QString>
#include <QVector>

class QPainter;

namespace kobalt {
class AssetBundle;
}
namespace kobalt::ui {
// Owns independent font bytes and faces on the GUI thread. It never shares
// CREngine's worker-owned font manager or changes Qt's font database.
bool initializePrivateInterfaceText(const AssetBundle &assets, bool opening = false);
// Count retained font buffers once, including collections with several faces.
qint64 privateInterfaceFontBytes();
enum class PrivateFontRegistration { Failed, Native, Private };
// Retain reading-font bytes before user storage is released. CFF and font
// collections stay on the private path, including their font-picker samples.
// forcePrivate also keeps TrueType previews out of Qt's font loader. Once a
// retained file uses private painting, later registrations keep that choice.
PrivateFontRegistration registerPrivateInterfaceFont(const QString &path, const QString &expectedFamily,
                                                     bool forcePrivate = false);
// A native editor needs a Qt face. Only single-face TrueType files are safe
// to pass to Qt's font loader; private previews keep their existing renderer.
QByteArray nativeEditorFontData(const QFont &font);
struct PrivateText {
    QImage image;
    int advance = 0;
    int height = 0;
    int missingGlyphs = 0;
    int lineAscent = 0;
    int lineHeight = 0;
    QPoint origin;
};
enum class PrivateTextEmphasis { Synthetic, FontFace };
struct PrivateTextStyle {
    int start = 0, length = 0;
    QFont font;
    QColor color;
    int baselineShift = 0;
    QImage image{};
    QColor background{};
};
// All application text uses private painting once font sources are initialized.
bool needsPrivateText(const QString &text, const QFont &font);
bool privateFontFixedPitch(const QFont &font);
PrivateText renderPrivateText(const QString &text, const QFont &font, int weight = -1,
                              const QVector<QPair<int, int>> &boldRanges = {},
                              const QVector<QPair<int, int>> &underlineRanges = {},
                              PrivateTextEmphasis emphasis = PrivateTextEmphasis::Synthetic,
                              Qt::LayoutDirection direction = Qt::LayoutDirectionAuto,
                              const QVector<PrivateTextStyle> &styles = {});
int measurePrivateText(const QString &text, const QFont &font, const QVector<QPair<int, int>> &boldRanges = {},
                       PrivateTextEmphasis emphasis = PrivateTextEmphasis::Synthetic,
                       Qt::LayoutDirection direction = Qt::LayoutDirectionAuto);
PrivateText measurePrivateTextLayout(const QString &text, const QFont &font,
                                     const QVector<QPair<int, int>> &boldRanges = {},
                                     PrivateTextEmphasis emphasis = PrivateTextEmphasis::Synthetic,
                                     Qt::LayoutDirection direction = Qt::LayoutDirectionAuto,
                                     const QVector<PrivateTextStyle> &styles = {});
QSize measurePrivateTextSize(const QString &text, const QFont &font);
QString elidePrivateText(const QString &text, const QFont &font, int width,
                         const QVector<QPair<int, int>> &boldRanges = {});
int interfaceTextWidth(const QString &text, const QFont &font);
int interfaceLineHeight(const QFont &font);
int interfaceLineAscent(const QFont &font);
QRect interfaceInkBounds(const QString &text, const QFont &font);
QString elideInterfaceText(const QString &text, const QFont &font, int width);
void drawInterfaceText(QPainter &painter, const QRect &area, const QString &text,
                       Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter, int weight = -1,
                       const QVector<QPair<int, int>> &boldRanges = {});
// Centers the ink of the letters vertically instead of the line box.
void drawOpticalPrivateText(QPainter &painter, const QRect &area, const QString &text,
                            Qt::Alignment alignment = Qt::AlignLeft, int weight = -1,
                            const QVector<QPair<int, int>> &boldRanges = {});
class PrivateTextLabel : public QLabel {
public:
    using QLabel::QLabel;
    void setMaximumLines(int value) { maximumLines = value; update(); }
    void setHighlight(const QString &style, QColor color, int spacing) {
        highlightStyle = style;
        highlightColor = color;
        lineSpacing = spacing;
        update();
    }
    int textWidth(const QString &value) const;
    QString elidedText(const QString &value, int width) const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int maximumLines = 0;
    QString highlightStyle;
    QColor highlightColor;
    int lineSpacing = 0;
};
}
