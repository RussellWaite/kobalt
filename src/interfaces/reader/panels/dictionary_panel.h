#pragma once
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/paging.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/controls/paged_text.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "platform/api/dictionary_service.h"
#include <QFont>
#include <QLabel>

namespace kobalt {
// A sheet across the full width, fixed to the screen edge away from the word.
class DictionaryPanel : public ui::Panel {
public:
    explicit DictionaryPanel(QWidget *parent);
    std::function<void()> dismissed;
    std::function<void(QString)> chosen;
    void setChoices(QVector<DictionaryChoice> values);
    void setLanguage(const QString &language);
    // The resolved book face supplies the header and default definition text.
    void setBookFont(const QString &family);
    static QFont definitionFont(const QString &family, int percent);
    static int definitionLineHeight(int percent, bool customFont = false);
    void setDefinitionFont(const QString &family, int percent);
    void open(const QString &word, const DictionaryResult &result, const QRect &selection);
    void place(const QRect &selection);
    static QString formatDefinition(const QString &html);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;

private:
    ui::PagerBar *pages, *choicesPaging;
    ui::CellButton *dictionary, *close;
    QLabel *headword;
    QString word, bookFamily, language, definitionFamily;
    int definitionPercent = 100;
    QFont headwordFont;
    QFont bookFont(int pixels) const;
    ui::Panel *dropdown;
    QVector<DictionaryChoice> options;
    QVector<QPushButton *> optionButtons;
    QString currentId;
    int optionsPage = 0, visibleRows = ui::metrics::lookup::choiceRows;
    bool lower = true;
    // Open at the page that holds the selected dictionary.
    void redrawChoices(bool atCurrent = false);
    ui::PagedText *text;
    int rowTop() const;
    int contentBottom() const;
    void arrange();
    void refresh();
};
}
