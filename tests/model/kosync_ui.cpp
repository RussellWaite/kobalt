#include "interfaces/reader/settings/settings_panel.h"
#include "check.h"
#include "support/strings.h"
#include "reader_inputs.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/styles.h"
#include <QApplication>
#include <QLineEdit>
#include <QFontInfo>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QVariant>
#include <QStyleFactory>

using namespace kobalt;
namespace {
class Keyboard final : public KeyboardController {
public:
    explicit Keyboard(int &destroyed, QWidget *host) : destroyed_(destroyed), host_(host) {}
    ~Keyboard() override { ++destroyed_; }
    bool show() override { place(); return true; }
    void hide() override { host_->window()->setProperty("reader_keyboard_bounds", QRect()); }
    void place() override { host_->window()->setProperty("reader_keyboard_bounds", QRect(host_->mapTo(host_->window(), QPoint(0, host_->height() / 2)), QSize(host_->width(), host_->height() - host_->height() / 2))); }
    void setSubmitText(const QString &text) override { submitText = text; }
    QString submitText;
private:
    int &destroyed_;
    QWidget *host_;
};
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create("Fusion"));
    app.setPalette(ui::readerPalette(app.palette(), false));
    require(ui::initializeInterfaceFonts(test::readerAssets()), "load the production interface fonts");
    QWidget window;
    window.setFont(ui::interfaceFont());
    window.resize(800, 1136);
    // Nickel applies its own button constraints before Kobalt opens.
    ui::readerStyle(&window, ui::styles::reader() + "QPushButton{min-height:60px;max-height:60px;}");
    int destroyed = 0;
    Keyboard *keyboard = nullptr;
    reader::SettingsPanelInputs inputs;
    inputs.keyboard = [&](QWidget *host, QLineEdit *) {
        auto controller = std::make_unique<Keyboard>(destroyed, host);
        keyboard = controller.get();
        return controller;
    };
    reader::SettingsPanel panel(&window, inputs);
    panel.resize(window.size());
    auto *tab = panel.findChild<QPushButton *>("rrSettingsKosync");
    require(tab && panel.settingsPages->widget(reader::KosyncSettings) == panel.kosync,
            "Sync has its own Settings tab on desktop");
    panel.show();
    window.show();
    tab->click();
    panel.fitContent(window.size());
    QApplication::processEvents();
    require(panel.settingsPages->currentIndex() == reader::KosyncSettings && tab->isChecked(),
            "Sync tab selects its page and check state");
    auto *about = panel.findChild<QPushButton *>("rrSettingsAbout");
    require(about && tab->mapTo(&panel, QPoint()).y() < about->mapTo(&panel, QPoint()).y(),
            "Sync appears above About in the Settings rail");
    auto *page = panel.kosync;
    page->setDeviceName({}, "Kobo Clara BW (Kobalt)");
    auto *server = page->findChild<QPushButton *>("rrKosyncServer");
    auto *password = page->findChild<QPushButton *>("rrKosyncPassword");
    auto *login = page->findChild<QPushButton *>("rrKosyncLogin");
    auto *automatic = page->findChild<QCheckBox *>("rrKosyncAutomatic");
    auto *sync = page->findChild<QPushButton *>("rrKosyncNow");
    auto *logout = page->findChild<QPushButton *>("rrKosyncLogout");
    require(server && password && login && automatic && sync && logout, "all sync controls exist");
    require(server->height() == ui::metrics::chrome::actionHeight &&
            password->height() == ui::metrics::chrome::actionHeight &&
            login->height() == ui::metrics::chrome::actionHeight &&
            sync->height() == ui::metrics::chrome::actionHeight,
            "account fields and actions keep Settings row heights instead of collapsing");
    require(!automatic->isEnabled() && !sync->isEnabled() && !logout->isEnabled(), "signed-out actions are disabled");
    require(page->server().isEmpty(), "fresh account has no prefilled server");
    server->click();
    require(page->editorDialog && keyboard, "server editor uses the host keyboard");
    require(keyboard->submitText == "OK", "account editor labels native submission as OK rather than Search");
    auto *editor = page->editorDialog->findChild<QLineEdit *>("rrKosyncInput");
    require(editor, "server editor is accessible");
    require(QFontInfo(editor->font()).family() == ui::interfaceFont().family(),
            "input inherits the production UI font rather than the system font");
    ui::ChoiceModal reference(&window, {"A regular alert.", "Alert", {}},
                              {"Cancel", "rrReferenceCancel"}, {"OK", "rrReferenceOK"});
    reference.fitCard();
    auto *card = page->editorDialog->card();
    auto *cancel = card->findChild<QPushButton *>("rrKosyncEditCancel");
    auto *save = card->findChild<QPushButton *>("rrKosyncEditSave");
    require(QFontInfo(save->font()).family() == ui::interfaceFont().family() &&
            QFontInfo(card->findChild<QLabel *>("rrModalTitle")->font()).family() == ui::interfaceFont().family(),
            "dialog title and actions inherit the production UI font");
    require(card->rows()->contentsMargins() == reference.card()->rows()->contentsMargins() &&
            card->rows()->spacing() == reference.card()->rows()->spacing(),
            "input dialog uses the regular alert frame margins and row spacing");
    require(cancel->geometry().left() == reference.secondaryButton()->geometry().left() &&
            save->geometry().right() == reference.primaryButton()->geometry().right() &&
            cancel->geometry().bottom() == card->height() - 3,
            "Cancel and OK dividers meet the alert frame without outer padding");
    require(editor->x() == cancel->x() && editor->width() == cancel->width() + save->width(),
            "input occupies the alert content band without an inset box");
    if (argc > 1) {
        reference.show();
        reference.raise();
        require(window.grab().save(QString::fromLocal8Bit(argv[1]) + "-alert.png"), "save regular alert reference");
        reference.hide();
    }
    require(page->editorDialog->card()->geometry().bottom() < window.height() / 2,
            "entire text entry card stays above the Kobo keyboard");
    if (argc > 1) require(window.grab().save(QString::fromLocal8Bit(argv[1]) + "-editor.png"), "save editor capture");
    for (const auto size : {QSize(1072, 1448), QSize(1448, 1072), QSize(800, 1136)}) {
        window.resize(size);
        panel.resize(size);
        panel.fitContent(size);
        QApplication::processEvents();
        require(page->editorDialog->card()->geometry().bottom() < size.height() / 2,
                "text entry remains above keyboard after Clara portrait and landscape resizes");
    }
    editor->setText("https://sync.example/prefix");

    keyboard->submitted();
    require(destroyed == 0, "keyboard survives its own submission callback");
    QApplication::processEvents();
    require(!page->editorDialog && destroyed == 1 && page->server() == "https://sync.example/prefix",
            "server edit commits and releases keyboard after submission");
    password->click();
    editor = page->editorDialog->findChild<QLineEdit *>("rrKosyncInput");
    require(editor->echoMode() == QLineEdit::Password && editor->inputMethodHints().testFlag(Qt::ImhSensitiveData),
            "password entry hides characters and disables keyboard learning");
    editor->setText("private-test-password");
    keyboard->submitted();
    QApplication::processEvents();
    QString submitted;
    page->loginRequested = [&] { submitted = page->takePassword(); };
    login->click();
    require(submitted == "private-test-password" && page->takePassword().isEmpty(), "login consumes and clears password");
    page->setAccount("https://sync.example/prefix", "reader", true, true);
    require(automatic->isEnabled() && automatic->isChecked() && sync->isEnabled() && logout->isEnabled(),
            "signed-in account enables sync controls");
    auto *lastSync = page->findChild<QLabel *>("rrKosyncLastSync");
    require(lastSync && lastSync->isVisible() && lastSync->text() == "Last synced: Never",
            "new account clearly has no recorded sync");
    page->setLastSync(QDateTime::fromString("2026-10-25T09:42:00Z", Qt::ISODate));
    require(lastSync->text().startsWith("Last synced: ") && !lastSync->text().contains("Never"),
            "successful sync has a local date and time");
    auto *deviceName = page->findChild<QPushButton *>("rrKosyncDeviceName");
    require(deviceName && deviceName->isVisible(), "sync device name is editable");
    QString renamed;
    page->deviceNameChanged = [&](const QString &value) { renamed = value; };
    deviceName->click();
    editor = page->editorDialog->findChild<QLineEdit *>("rrKosyncInput");
    require(editor->placeholderText() == "Kobo Clara BW (Kobalt)" && editor->maxLength() == 128 &&
            page->editorDialog->card()->geometry().bottom() < window.height() / 2,
            "device name editor shows its default and stays above the keyboard");
    editor->setText("Living room Kobo");
    keyboard->submitted();
    QApplication::processEvents();
    require(renamed == "Living room Kobo" && automatic->isEnabled() && !login->isVisible(),
            "renaming reaches the owner without requiring account authentication");
    deviceName->click();
    page->editorDialog->findChild<QLineEdit *>("rrKosyncInput")->clear();
    keyboard->submitted();
    QApplication::processEvents();
    require(renamed.isEmpty(), "empty device name requests the automatic default");
    auto *matching = dynamic_cast<ui::CaptionButton *>(page->findChild<QPushButton *>("rrKosyncMatching"));
    require(matching && matching->caption() == "Binary (default)", "matching defaults to Binary");
    int matchingChanges = 0;
    page->filenameMatchingChanged = [&](bool filename) { ++matchingChanges; page->setFilenameMatching(filename); };
    matching->click();
    auto *matchingDialog = dynamic_cast<ui::NoticeModal *>(window.findChildren<QWidget *>("rrKosyncMatchingDialog").last());
    require(matchingDialog && matchingDialog->isVisible() && window.rect().contains(matchingDialog->card()->geometry()),
            "matching selector fits on the Kobo screen");
    if (argc > 1) require(window.grab().save(QString::fromLocal8Bit(argv[1]) + "-matching.png"), "save document matching selector");
    auto *filename = matchingDialog->findChild<QAbstractButton *>("rrKosyncFilename");
    require(filename, "filename option is available");
    filename->click();
    require(matchingChanges == 1 && matching->caption() == "Filename" && !matchingDialog->isVisible(), "filename selection reaches the owner");
    page->setStatus(strings::get("kosync.matching_changed"), false);
    panel.fitContent(window.size());
    QApplication::processEvents();
    require(lastSync->visibleRegion().boundingRect() == lastSync->rect(), "last sync remains visible after a matching change");
    matching->click();
    matchingDialog = dynamic_cast<ui::NoticeModal *>(window.findChildren<QWidget *>("rrKosyncMatchingDialog").last());
    matchingDialog->primaryButton()->click();
    require(matchingChanges == 1 && matching->caption() == "Filename", "Cancel leaves matching unchanged");
    matching->click();
    matchingDialog = dynamic_cast<ui::NoticeModal *>(window.findChildren<QWidget *>("rrKosyncMatchingDialog").last());
    matchingDialog->findChild<QAbstractButton *>("rrKosyncBinary")->click();
    require(matchingChanges == 2 && matching->caption() == "Binary (default)", "binary selection can be restored");
    matching->click();
    QPointer<QWidget> dismissedMatching = window.findChildren<QWidget *>("rrKosyncMatchingDialog").last();
    page->dismissEditor();
    require(!dismissedMatching || !dismissedMatching->isVisible(), "sleep cleanup dismisses matching choices");
    page->setStatus("Syncing progress…", true);
    require(!login->isEnabled() && !server->isEnabled() && !automatic->isEnabled() && !matching->isEnabled(), "busy state blocks overlapping edits");
    page->setStatus("Reading progress is up to date.", false);
    panel.fitContent(window.size());
    QApplication::processEvents();
    QElapsedTimer settled;
    settled.start();
    while (logout->visibleRegion().boundingRect() != logout->rect() && settled.elapsed() < 1000)
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    require(!password->isVisible() && !login->isVisible(), "signed-in page hides sign-in controls");
    require(sync->visibleRegion().boundingRect() == sync->rect() &&
            logout->visibleRegion().boundingRect() == logout->rect(), "signed-in controls fit on one page");
    require(lastSync->visibleRegion().boundingRect() == lastSync->rect(), "last sync fits on the signed-in page");
    if (argc > 1) require(window.grab().save(QString::fromLocal8Bit(argv[1])), "save account settings capture");
    page->setNativeCloudSync(true);
    page->setAccount("https://sync.example/prefix", "reader", true, true);
    page->setStatus("Reading progress is up to date.", false);
    panel.fitContent(window.size());
    QApplication::processEvents();
    auto *disclaimer = page->findChild<QLabel *>("rrKosyncDescription");
    require(tab->isVisible() && disclaimer && disclaimer->isVisible() &&
            disclaimer->text().contains("Kobo’s built-in sync system") &&
            page->findChild<QLabel *>("rrKosyncSideloadGuidance")->text().contains("DRM-free copy"), "Kobo-managed books retain a Sync tab with a sideloading disclaimer");
    for (const auto *control : {server, page->findChild<QPushButton *>("rrKosyncUsername"), password, login,
                                static_cast<QPushButton *>(sync), logout, deviceName, static_cast<QPushButton *>(matching)})
        require(control && !control->isVisible(), "Kobo-managed books hide all account and sync actions");
    require(!automatic->isVisible() && !lastSync->isVisible() &&
            !page->findChild<QLabel *>("rrKosyncStatus")->isVisible(), "Kobo-managed books hide automatic sync and account status");
    require(disclaimer->visibleRegion().boundingRect() == disclaimer->rect(), "Kobo sync disclaimer fits on one page");
    if (argc > 1) require(window.grab().save(QString::fromLocal8Bit(argv[1]) + "-kobo-managed.png"), "save Kobo-managed disclaimer capture");
    auto *guidance = page->findChild<QLabel *>("rrKosyncSideloadGuidance");
    auto *learnMore = page->findChild<QPushButton *>("rrKosyncLearnMore");
    require(guidance->isVisible() && learnMore && learnMore->isVisible() &&
            guidance->geometry().top() >= disclaimer->geometry().bottom(), "sideloading guidance and Learn More sit below the disclaimer");
    learnMore->click();
    auto *help = dynamic_cast<ui::NoticeModal *>(window.findChild<QWidget *>("rrKosyncHelp"));
    require(help && help->isVisible() && help->descriptionLabel()->text().contains("Only reading progress"), "Learn More explains Kobo ownership and KOsync scope");
    require(window.rect().contains(help->card()->geometry()), "sync explanation fits in the screen");
    if (argc > 1) require(window.grab().save(QString::fromLocal8Bit(argv[1]) + "-learn-more.png"), "save sync explanation capture");
    help->primaryButton()->click();
    require(!help->isVisible(), "OK dismisses the sync explanation");
    learnMore->click();
    QPointer<QWidget> reopenedHelp = window.findChildren<QWidget *>("rrKosyncHelp").last();
    require(reopenedHelp && reopenedHelp->isVisible(), "sync explanation can be reopened");
    panel.settingsPages->setCurrentIndex(reader::DisplaySettings);
    require(!reopenedHelp || !reopenedHelp->isVisible(), "leaving Sync dismisses its explanation");
    tab->click();
    page->setNativeCloudSync(false);
    page->setAccount("https://sync.example/prefix", "reader", true, true);
    require(server->isVisible() && sync->isVisible() && automatic->isVisible(), "sideloaded books retain normal KOsync controls");
    bool autoChanged = false;
    page->automaticChanged = [&](bool value) { autoChanged = !value; };
    automatic->click();
    require(autoChanged, "automatic sync toggle reaches the owner");
    page->setAccount({}, {}, false, false);
    panel.fitContent(window.size());
    QApplication::processEvents();
    password->click();
    require(page->editorDialog, "signed-out password editor opens");
    panel.settingsPages->setCurrentIndex(reader::DisplaySettings);
    require(!page->editorDialog && page->takePassword().isEmpty(), "leaving Sync dismisses keyboard and clears secrets");
    return 0;
}
