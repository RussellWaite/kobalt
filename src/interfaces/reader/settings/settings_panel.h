#pragma once
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/controls/paging.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "dictionaries_page.h"
#include "assets/font_catalog.h"
#include <QStackedWidget>
#include <QVector>
#include <QPointer>

namespace kobalt::reader {
enum SettingsPage {
    DisplaySettings,
    ControlsSettings,
    DefaultsSettings,
    StylingSettings,
    DictionariesSettings,
    SystemSettings,
    AboutSettings,
    NativeUiSettings
};
struct SettingsPanelInputs {
    QString release, commit;
    int qtMajor = 0;
    bool commitVisible = false;
    bool animationsSupported = false, lightSupported = false, warmthSupported = false;
    bool debugProgressAvailable = false, desktopUi = false;
    bool downloadableDictionaries = true;
    bool sleepScreenSupported = false;
    bool automaticBookOpeningSupported = false;
};
// Qt parents own every widget. Reader installs policy callbacks after construction.
class SettingsPanel final : public ui::Panel {
public:
    SettingsPanel(QWidget *parent, const SettingsPanelInputs &inputs);
    ~SettingsPanel() override;
    void dismissRestore();
    void showFontChoices(const QString &title, const QVector<ReadingFont> &fonts, const QString &current,
                         const QString &defaultFamily, std::function<void(const QString &)> chosen,
                         std::function<void(const QStringList &)> prepareFonts = {});
    void dismissFontChoices();
    void showHeaderFooterWeights(int current, const QFont &sampleFont, std::function<void(int)> chosen);
    void showColumnSpacingChoices(int current, std::function<void(int)> chosen);
    static QString columnSpacingLabel(int percent);
    enum class AdvancedText { WordSpacing, Tracking, Pagination };
    void showAdvancedTextChoices(AdvancedText feature, int current, std::function<void(int)> chosen,
                                 const QFont &sampleFont = {}, const QString &alignment = {});
    QPointer<ui::ModalOverlay> fontDialog;
    ui::CellButton *closeButton = nullptr;
    void showDictionaries(bool visible);
    void showNativeUi(bool visible);
    int fitContent(QSize availableSize);
    ui::DescribedOption *hideStatusOption = nullptr, *hideTabLabelsOption = nullptr;
    QStackedWidget *settingsPages = nullptr;
    QVector<QPushButton *> settingsTabButtons;
    ui::DescribedOption *darkOption = nullptr, *inheritTextLayoutOption = nullptr, *animationOption = nullptr;
    ui::DescribedOption *edgeLightOption = nullptr, *denseGraphOption = nullptr;
    ui::DescribedOption *linkIndicatorOption = nullptr, *hideNonlinearOption = nullptr,
                        *avoidWastefulStylingOption = nullptr;
    ui::DescribedOption *automaticOpeningOption = nullptr, *minimalAnnotationOption = nullptr;
    ui::CaptionButton *defaultFontButton = nullptr, *uiFontButton = nullptr, *uiMonoFontButton = nullptr,
                      *headerFooterFontButton = nullptr, *headerFooterWeightButton = nullptr;
    ui::CaptionButton *serifFontButton = nullptr, *sansFontButton = nullptr, *monoFontButton = nullptr;
    ui::DescribedOption *landscapeColumnsOption = nullptr;
    ui::CaptionButton *columnSpacingButton = nullptr;
    ui::CaptionButton *wordSpacingButton = nullptr, *trackingButton = nullptr, *widowsOrphansButton = nullptr;
    ui::CaptionButton *sleepScreenButton = nullptr, *sleepTileButton = nullptr, *menuViewButton = nullptr;
    ui::CaptionButton *pageNavigationButton = nullptr, *tapZonesButton = nullptr, *navigationBarButton = nullptr,
                      *tappableLinksButton = nullptr;
    ui::DescribedOption *bionicOption = nullptr;
    ui::CaptionButton *refreshButton = nullptr, *resetDefaults = nullptr;
    QPushButton *keepDefaults = nullptr, *confirmDefaults = nullptr;
    QPushButton *openAcknowledgments = nullptr, *debugProgress = nullptr;
    DictionariesPage *dictionaries = nullptr;
    QLabel *settingsError = nullptr;

protected:
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QWidget *tabRail = nullptr;
    QWidget *optionsViewport = nullptr, *optionsClip = nullptr;
    ui::PagerBar *optionsPager = nullptr;
    QVector<QPair<int, int>> optionPages;
    int optionPage = 0, pageHeight = 0;
    void showOptionsPage(int page);
    void fitRail(int availableHeight = 0);
    QLabel *aboutDescription = nullptr;
    QPointer<ui::ChoiceModal> restoreDialog;
};
}
