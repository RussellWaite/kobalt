#pragma once
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "platform/api/keyboard_controller.h"
#include <QPointer>
#include <QDateTime>

namespace kobalt::reader {
class KosyncPage final : public QWidget {
public:
    KosyncPage(QWidget *parent, QWidget *overlayParent, KeyboardFactory keyboard);
    ~KosyncPage() override;
    void setAccount(const QString &server, const QString &username, bool signedIn, bool automatic);
    void setNativeCloudSync(bool enabled);
    void setLastSync(const QDateTime &time);
    void setDeviceName(const QString &name, const QString &defaultName);
    void setFilenameMatching(bool enabled);
    void setManualExplanation(bool enabled);
    void setStatus(const QString &status, bool busy);
    QString server() const { return server_; }
    QString username() const { return username_; }
    QString takePassword();
    ui::DescribedOption *automaticOption() const { return automatic_; }
    ui::DescribedOption *manualExplanationOption() const { return manualExplanation_; }
    void dismissEditor();
    QPointer<ui::ModalOverlay> editorDialog;
    std::function<void()> loginRequested, logoutRequested, syncRequested;
    std::function<void(bool)> automaticChanged, manualExplanationChanged;
    std::function<void(const QString &)> deviceNameChanged;
    std::function<void(bool)> filenameMatchingChanged;

protected:
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void edit(int field);
    void placeEditor();
    void maskPassword();
    void updateCaptions();
    void showSyncHelp();
    void dismissSyncHelp();
    void showMatchingChoices();
    void dismissMatchingChoices();
    QPointer<ui::NoticeModal> syncHelp_;
    QPointer<ui::NoticeModal> matchingDialog_;
    QWidget *overlayParent_;
    KeyboardFactory makeKeyboard_;
    std::unique_ptr<KeyboardController> keyboard_;
    QString server_, username_, password_, deviceName_, defaultDeviceName_;
    bool signedIn_ = false;
    bool submitting_ = false;
    bool accountEdited_ = false;
    bool nativeCloudSync_ = false;
    bool filenameMatching_ = false;
    ui::CaptionButton *serverButton_, *usernameButton_, *passwordButton_, *deviceNameButton_, *matchingButton_;
    QPushButton *login_, *logout_, *sync_, *learnMore_;
    ui::DescribedOption *automatic_, *manualExplanation_;
    QLabel *description_, *sideloadGuidance_, *status_, *lastSync_;
    QWidget *syncInformation_;
};
}
