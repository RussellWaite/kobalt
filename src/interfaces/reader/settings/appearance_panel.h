#pragma once
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/controls/sliders.h"
#include "header_footer_panel.h"
#include "interfaces/reader/ui/controls/options.h"
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace kobalt::reader {
// The sheet's pages, in this order. Header and footer is a full-screen view;
// the reader supplies its rows in AppearanceLayout.
enum AppearancePage { TypographyPage, LayoutPage };
struct AppearanceLayout {
    QWidget *typography = nullptr, *pageLayout = nullptr, *headerFooter = nullptr;
    QVBoxLayout *typeRows = nullptr, *pageRows = nullptr, *headerFooterRows = nullptr;
};
struct AppearancePanelInputs {
    bool sideButtons = false, reverseLandscapeButtons = false;
    bool automaticRotationAvailable = false;
    bool systemRotation = false;
};
// Measure only the selected page. Overflow scrolls inside the available reader area.
class AppearanceSheet final : public ui::Panel {
public:
    explicit AppearanceSheet(QWidget *parent);
    AppearanceLayout controls() const;
    int fitHeight(int width, int availableHeight);
    QStackedWidget *pages;
    QLabel *message;

private:
    QScrollArea *scroll;
};
class StripSizeControl final : public QWidget {
public:
    StripSizeControl(QWidget *parent, bool footer);
    void setSize(int pixels, bool shown);
    ui::SettingSlider *slider;

private:
    void showValue(int value);
    QLabel *valueLabel;
};

// The existing pages own the controls. Reader keeps preview and apply policy.
class AppearancePanel {
public:
    AppearancePanel(const AppearanceLayout &layout, const AppearancePanelInputs &inputs);
    ui::FontButton *fontButton = nullptr;
    ui::SettingSlider *opticalSizeSlider = nullptr, *sizeSlider = nullptr, *weightSlider = nullptr,
                      *variableWeightSlider = nullptr, *spacingSlider = nullptr, *marginSlider = nullptr,
                      *verticalMarginSlider = nullptr;
    QVector<QPushButton *> alignmentButtons, hyphenButtons, rotationButtons;
    QPushButton *autoRotateOption = nullptr;
    // Header and footer: the Layout row that opens its view, and the view's
    // presets and controls, built only when the layout has its rows.
    ui::DropdownButton *headerFooterRow = nullptr;
    SlotMap *slotMap = nullptr;
    QVector<PresetCard *> presetCards;
    StripSizeControl *headerSizeControl = nullptr, *footerSizeControl = nullptr;
    QVector<QPushButton *> progressBarButtons;
    QLabel *slotHint = nullptr;
};
}
