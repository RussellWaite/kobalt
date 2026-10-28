#include "style_sample.h"
#include "engine_worker.h"
#include "engine/crengine/engine_activity.h"
#include "appearance_settings.h"
#include "reading_density.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QPointer>
#include <atomic>
#include <memory>

namespace kobalt {
namespace {
// Short paragraphs of different lengths. The heading keeps with the next
// paragraph, which leaves an ordinary gap at the bottom of some pages.
const char *const sampleBlocks[] = {
    "<p>The foundry kept hours of its own, and the clocks along the east wall had long since stopped agreeing "
    "with the town. Nobody minded very much. Work began when the first proof was dry and ended when the last one "
    "was.</p>",
    "<p>Ada sorted the returned specimens into three piles: those the printers loved, those they would tolerate, "
    "and those that had come back with angry notes pencilled in the margin.</p>",
    "<p>The third pile was the interesting one. A complaint about a lowercase g could easily run to four pages.</p>",
    "<p>“This one again,” said Tom, holding up an envelope with a familiar postmark.</p>",
    "<p>“Open it,” said Ada. “It cannot be worse than the last one.”</p>",
    "<p>It was. The customer had mailed back an entire alphabet, every single letter circled twice in red ink, "
    "with no further comment of any kind.</p>",
    "<p>Ada pinned it above her desk. It was, she said, the most honest review the foundry had ever had.</p>",
    "<p>The machines had opinions too. They jammed on italics, sulked through long runs of capitals, and ran "
    "best late at night when nobody was there to watch them work.</p>",
    "<p>“They like the dark,” Tom said. “So do I, most days.”</p>",
    "<p>By spring the red alphabet had faded to pink, and the new specimen was ready for the printers to "
    "judge.</p>",
    "<h2>II</h2>",
    "<p>The new specimen went out in April. It was printed on heavier paper than usual, so that the printers "
    "would take it seriously.</p>",
    "<p>It carried a short note from Ada explaining what had changed and what had not. The lowercase g was "
    "rounder. The capitals were a little lighter.</p>",
    "<p>Three weeks passed before the first reply. It came from the same customer, in the same envelope.</p>",
    "<p>“Well?” said Tom.</p>",
    "<p>This time only one letter was circled, and only once. Beside it, in very small writing, someone had "
    "added a single word.</p>",
    "<p>Ada read it several times before she was sure of it, then pinned the sheet next to the first one.</p>",
    "<p>“Good,” Tom read aloud. “Just that?”</p>",
    "<p>“Just that,” said Ada, and went back to work at the long table under the windows.</p>",
    "<p>The clocks along the east wall went on disagreeing with the town, and with each other, for many years "
    "after that.</p>",
    "<p>Nobody minded very much. The foundry kept hours of its own, and the work was good.</p>",
};
constexpr int sampleBlockCount = int(sizeof(sampleBlocks) / sizeof(sampleBlocks[0]));
// Pages after these rarely show anything the first spreads do not.
constexpr int searchedPages = 10;
// Whether a page break falls inside a paragraph depends on the text size. When
// the options lay out the sample the same way, it starts at a later paragraph
// instead, which moves every break.
constexpr int sampleVariants = 6;

std::shared_ptr<ResourceCache> sampleBook(int variant, QMap<QString, qint64> *sizes) {
    QByteArray body;
    for (int index = 0; index < sampleBlockCount; ++index)
        body += sampleBlocks[(index + variant * 3) % sampleBlockCount];
    const QMap<QString, QByteArray> files{
        {"mimetype", "application/epub+zip"},
        {"META-INF/container.xml",
         "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles>"
         "<rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>"},
        {"book.opf",
         "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Sample</title>"
         "<language xmlns='http://purl.org/dc/elements/1.1/'>en</language></metadata><manifest>"
         "<item id='c' href='c.xhtml' media-type='application/xhtml+xml'/></manifest>"
         "<spine><itemref idref='c'/></spine></package>"},
        {"c.xhtml", "<html xmlns='http://www.w3.org/1999/xhtml'><head><style>"
                    "p{margin:0;text-indent:1.2em}h2{margin:1em 0;text-align:center;"
                    "font-size:1em;font-weight:normal;letter-spacing:.2em}"
                    "</style></head><body>" +
                        body + "</body></html>"}};
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        (*sizes)[it.key()] = it.value().size();
    return std::make_shared<ResourceCache>(
        files.keys(),
        [files](const QString &name, const ResourceCache::Sink &sink, QString *) {
            const auto bytes = files.value(name);
            return sink(bytes.constData(), bytes.size());
        },
        std::make_shared<std::atomic_bool>(false));
}
bool cancelled(const StyleSampleRequest &request) {
    engineActivity().checkpoint();
    return request.cancelled && request.cancelled->load();
}
// Applying an option is the costly step, so it also checks for cancellation.
bool apply(ReaderModel &model, const StyleSampleRequest &request, const QJsonValue &value) {
    if (cancelled(request))
        return false;
    // Qt 5.2 has no initializer-list constructor for JSON objects.
    QJsonObject settings;
    settings.insert(request.key, value);
    return model.applyAppearance(settings);
}
QStringList pageStarts(ReaderModel &model) {
    QStringList starts;
    for (int page = 0; page < qMin(model.pages(), searchedPages + 1); ++page)
        starts.append(model.pageStart(page));
    return starts;
}
// The first page whose start differs between options, so the spread before it
// shows the changed break. Returns -1 when every option breaks the same way.
int firstChangedBreak(const QVector<QStringList> &starts) {
    for (int page = 1; page < starts.first().size(); ++page)
        for (const auto &option : starts)
            if (option.value(page) != starts.first().value(page))
                return page;
    return -1;
}
// The first page that the options draw differently, or -1. Page spacing moves
// lines within a page without changing where pages start.
int firstChangedPage(ReaderModel &model, const StyleSampleRequest &request) {
    for (int page = 0; page < qMin(model.pages(), searchedPages); ++page) {
        QImage first;
        for (const auto &value : request.values) {
            if (!apply(model, request, value))
                return -1;
            const auto image = model.image(page);
            if (first.isNull())
                first = image;
            else if (image != first)
                return page;
        }
    }
    return -1;
}
// The height that ends a crop halfway through the gap below its last whole
// line, found from the rendered pixels. Rows that differ from the paper
// colour hold ink.
int wholeLineHeight(const QImage &crop) {
    const auto paper = qGray(crop.pixel(0, 0));
    const auto inked = [&crop, paper](int y) {
        for (int x = 0; x < crop.width(); ++x)
            if (qAbs(qGray(crop.pixel(x, y)) - paper) > 64)
                return true;
        return false;
    };
    int y = crop.height() - 1;
    // Skip a line cut by the bottom edge, then find the last line's bottom.
    while (y > 0 && inked(y))
        --y;
    const int gapBottom = y;
    while (y > 0 && !inked(y))
        --y;
    return y > 0 ? y + 1 + (gapBottom - y) / 2 : crop.height();
}
// The vertical extent of each line of ink in `area`.
QVector<QPair<int, int>> lineBands(const QImage &page, const QRect &area) {
    const auto paper = qGray(page.pixel(0, 0));
    QVector<QPair<int, int>> bands;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        bool inked = false;
        for (int x = area.left(); x <= area.right() && !inked; ++x)
            inked = qAbs(qGray(page.pixel(x, y)) - paper) > 64;
        if (!inked)
            continue;
        // Accents and dots a pixel or two above their letters join their line.
        if (!bands.isEmpty() && y - bands.last().second <= 2)
            bands.last().second = y;
        else
            bands.append({y, y});
    }
    return bands;
}
// Height that `bands` leave empty compared with `baseline`, the same lines
// drawn by the first option: above the first line, between lines and below
// the last line down to the end of the reading area.
QVector<QRect> spareSpace(const QVector<QPair<int, int>> &bands, const QVector<QPair<int, int>> &baseline,
                          const QRect &area) {
    QVector<QRect> spare;
    if (bands.isEmpty() || bands.size() != baseline.size())
        return spare;
    const auto band = [&area](int top, int bottom) { return QRect(area.left(), top, area.width(), bottom - top); };
    if (bands.first().first - baseline.first().first > 1)
        spare.append(band(baseline.first().first, bands.first().first));
    for (int index = 1; index < bands.size(); ++index) {
        const int added = (bands[index].first - bands[index - 1].first) -
                          (baseline[index].first - baseline[index - 1].first);
        if (added > 1)
            spare.append(band(bands[index].first - added, bands[index].first));
    }
    // A full line's pitch below the last line is still text height.
    const int pitch = baseline.size() > 1 ? (baseline.last().first - baseline.first().first) / (baseline.size() - 1)
                                          : baseline.last().second - baseline.last().first;
    const int end = bands.last().first + pitch;
    if (area.bottom() + 1 - end > 1)
        spare.append(band(end, area.bottom() + 1));
    return spare;
}
StyleSample render(const EngineFontInputs &fonts, const StyleSampleRequest &request) {
    StyleSample result;
    if (request.values.isEmpty() || request.page.isEmpty())
        return result;
    auto settings = request.settings;
    settings[request.key] = request.values.first();
    const bool pages = request.crop.isEmpty();
    std::unique_ptr<ReaderModel> model;
    int page = 0;
    result.differs = false;
    for (int variant = 0; variant < (pages ? sampleVariants : 1) && !result.differs; ++variant) {
        if (cancelled(request))
            return {};
        model = std::make_unique<ReaderModel>(fonts);
        QMap<QString, qint64> sizes;
        QString error;
        if (!model->openResources(sampleBook(variant, &sizes), sizes, request.page, settings, &error) ||
            model->pages() < 1)
            return {};
        if (!pages) {
            result.differs = true;
            break;
        }
        QVector<QStringList> starts;
        for (const auto &value : request.values) {
            if (!apply(*model, request, value))
                return {};
            starts.append(pageStarts(*model));
        }
        const int changedBreak = firstChangedBreak(starts);
        const int changedPage = changedBreak < 0 ? firstChangedPage(*model, request) : -1;
        result.differs = changedBreak > 0 || changedPage >= 0;
        page = changedBreak > 0 ? changedBreak - 1 : qMax(0, changedPage);
    }
    // Keep two pages on screen even when the change is on the last page.
    result.page = qMax(0, qMin(page, model->pages() - 2));
    const QRect area = request.readingArea.intersected(QRect(QPoint(), request.page));
    const bool detail = !request.detail.isEmpty() && !area.isEmpty();
    // The bottom edge of the reading area, with a little paper beside and below.
    const int side = qMax(0, qMin(area.left(), request.page.width() - 1 - area.right()));
    QRect view = QRect(area.left() - side, area.bottom() + 1 + side - request.detail.height(),
                       area.width() + 2 * side, request.detail.height())
                     .intersected(QRect(QPoint(), request.page));
    if (!request.page.isEmpty()) {
        const qreal scale = qreal(request.thumbnail.width()) / request.page.width();
        result.readingArea = QRectF(area.topLeft() * scale, area.size() * scale);
    }
    QVector<QPair<int, int>> baseline;
    for (const auto &value : request.values) {
        if (!apply(*model, request, value))
            return {};
        QVector<QImage> spread;
        for (int index = result.page; index < qMin(result.page + 2, model->pages()); ++index) {
            const auto image = model->image(index);
            if (index == 0 && !pages)
                result.crops.append(image.copy(QRect(request.cropOrigin, request.crop)));
            if (detail && index == page) {
                const auto bands = lineBands(image, area);
                if (baseline.isEmpty()) {
                    baseline = bands;
                    // Start the view in the gap above its first whole line.
                    for (int line = 1; line < bands.size(); ++line)
                        if (bands[line].first >= view.top()) {
                            view.setTop((bands[line - 1].second + bands[line].first) / 2);
                            break;
                        }
                    result.detailArea = area.translated(-view.topLeft());
                }
                QVector<QRect> spare;
                for (const auto &rect : spareSpace(bands, baseline, area))
                    spare.append(rect.translated(-view.topLeft()).intersected(QRect(QPoint(), view.size())));
                result.details.append(image.copy(view));
                result.spare.append(spare);
            }
            // Scale right away so only one full page is held at a time.
            spread.append(image.scaled(request.thumbnail, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        result.spreads.append(spread);
    }
    if (!pages) {
        // One height for every option keeps the preview steady while choosing.
        const int height = wholeLineHeight(result.crops.first());
        for (auto &crop : result.crops)
            crop = crop.copy(0, 0, crop.width(), height);
        result.differs = false;
        for (const auto &crop : result.crops)
            result.differs = result.differs || crop != result.crops.first();
    }
    return result;
}
}

QJsonObject styleSampleSettings(QJsonObject state) {
    for (const auto *key : {"position", "dom_version", "imported_native_location", "language", "pages"})
        state.remove(key);
    // The sample shows one column, even when the book uses two in landscape.
    state["columns"] = 1;
    state["two_columns_landscape"] = false;
    return state;
}
QSize styleSamplePageSize() {
    // A six-inch page at 300 ppi, scaled to the screen's density.
    return {readingPixels(1072), readingPixels(1448)};
}
QRect styleSampleReadingArea() {
    const auto page = styleSamplePageSize();
    const int side = defaultMarginPixels(), top = readingPixels(36), bottom = readingPixels(36);
    return {side, top, page.width() - 2 * side, page.height() - top - bottom};
}
QJsonObject styleSamplePageSettings(const QJsonObject &reading) {
    const auto area = styleSampleReadingArea();
    QJsonObject settings;
    for (const auto *key : {"dark_mode", "widows_orphans", "vertical_justification", "vertical_spacing_scope"})
        if (reading.contains(key))
            settings[key] = reading.value(key);
    settings["font_face"] = QString(defaultFontFace);
    settings["default_font_face"] = QString(defaultFontFace);
    settings["font_pixels"] = defaultFontPixels();
    settings["line_spacing"] = 140;
    settings["line_spacing_version"] = 1;
    settings["margin_pixels"] = area.left();
    settings["top_margin_pixels"] = area.top();
    settings["bottom_margin_pixels"] = styleSamplePageSize().height() - area.bottom() - 1;
    settings["text_alignment"] = QStringLiteral("justify");
    settings["hyphenation"] = QStringLiteral("auto");
    settings["weight_steps"] = 0;
    settings["bold_reading"] = QStringLiteral("off");
    settings["word_spacing"] = 100;
    settings["justification_tracking"] = 0;
    settings["avoid_wasteful_styling"] = true;
    settings["columns"] = 1;
    settings["two_columns_landscape"] = false;
    return settings;
}
void renderStyleSample(EngineFontInputs fonts, StyleSampleRequest request, QObject *context,
                       std::function<void(StyleSample)> finished) {
    QPointer<QObject> guard(context);
    // The sample model shares CREngine's global fonts and caches, so it is
    // created, used and destroyed on the reader's engine worker.
    kobalt::qt::invoke(
        engineWorker(),
        [guard, fonts = std::move(fonts), request = std::move(request), finished = std::move(finished)] {
            StyleSample result;
            {
                EngineJob activity;
                result = render(fonts, request);
            }
            kobalt::qt::invoke(
                qApp,
                [guard, finished, result = std::move(result)]() mutable {
                    if (guard)
                        finished(std::move(result));
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
}
