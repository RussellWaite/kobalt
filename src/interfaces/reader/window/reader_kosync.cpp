#include "reader_p.h"
#include "support/strings.h"
#include <QTimer>
#include <QHBoxLayout>
#include <QStyle>
#include "interfaces/reader/ui/style/styles.h"
#include <utility>
#include <QLocale>

namespace kobalt::reader {
QByteArray Reader::kosyncDocument() const {
    return kosyncClient->filenameMatching() ? model->syncFilenameDigest() : model->syncDigest();
}
void Reader::kosyncStatus(const QString &message) {
    kosyncMessage = message;
    kosyncPage->setLastSync(kosyncClient->lastSync());
    kosyncPage->setStatus(message, kosyncClient->busy());
    if (general->isVisible() && !lifecycleHidden && !closing && !exportRunning) {
        fitSettings();
        refresh();
    }
}
void Reader::buildKosync() {
    kosyncClient = std::make_unique<kosync::Client>(book->paths().syncAccount());
    kosyncClient->setDefaultDeviceName(services.deviceName.isEmpty() ? QStringLiteral("Kobalt")
                                                                 : services.deviceName + QStringLiteral(" (Kobalt)"));
    kosyncPage->setNativeCloudSync(book->usesNativeCloudSync());
    const auto account = [this] {
        kosyncPage->setFilenameMatching(kosyncClient->filenameMatching());
        kosyncPage->setDeviceName(kosyncClient->customDeviceName(), kosyncClient->defaultDeviceName());
        kosyncPage->setAccount(kosyncClient->server(), kosyncClient->username(), kosyncClient->signedIn(),
                               kosyncClient->automatic());
    };
    account();
    kosyncPage->filenameMatchingChanged = [this](bool enabled) {
        if (enabled == kosyncClient->filenameMatching()) return;
        const bool saved = kosyncClient->setFilenameMatching(enabled);
        if (saved) {
            cancelKosync();
            kosyncProgress = {};
            kosyncPositionFailed = false;
            kosyncPromptDeferred = false;
            kosyncStarted = false;
        }
        kosyncPage->setFilenameMatching(kosyncClient->filenameMatching());
        kosyncStatus(strings::get(!saved ? "kosync.storage_error" :
            enabled && model->syncFilenameDigest().isEmpty() ? "kosync.filename_unavailable" : "kosync.matching_changed"));
    };
    kosyncPage->deviceNameChanged = [this](const QString &name) {
        const bool saved = kosyncClient->setDeviceName(name);
        kosyncPage->setDeviceName(kosyncClient->customDeviceName(), kosyncClient->defaultDeviceName());
        kosyncStatus(strings::get(saved ? "kosync.device_name_saved" : "kosync.storage_error"));
    };
    kosyncPage->loginRequested = [this, account] {
        if (kosyncClient->busy()) return;
        kosyncClient->login(kosyncPage->server(), kosyncPage->username(), kosyncPage->takePassword(),
                            [this, account](const QString &error) {
            if (error.isEmpty()) {
                ++kosyncEpoch;
                kosyncProgress = {};
                kosyncPositionFailed = false;
                kosyncPromptDeferred = false;
                kosyncStarted = false;
                account();
            }
            kosyncStatus(error.isEmpty() ? strings::get("kosync.signed_in") : error);
        });
        if (kosyncClient->busy()) kosyncStatus(strings::get("kosync.signing_in"));
    };
    kosyncPage->logoutRequested = [this, account] {
        ++kosyncEpoch;
        if (kosyncClient->logout()) {
            kosyncProgress = {};
            kosyncPositionFailed = false;
            kosyncPromptDeferred = false;
            kosyncStarted = false;
            account();
            kosyncStatus(strings::get("kosync.signed_out"));
        } else kosyncStatus(strings::get("kosync.storage_error"));
    };
    kosyncPage->automaticChanged = [this, account](bool enabled) {
        if (!kosyncClient->setAutomatic(enabled)) kosyncStatus(strings::get("kosync.storage_error"));
        account();
        if (enabled) kosyncStarted = false;
    };
    kosyncPage->syncRequested = [this] { synchronizeKosync(true); };
    const auto status = book->usesNativeCloudSync() ? strings::get("kosync.native_sync")
        : strings::get(kosyncClient->signedIn() ? "kosync.signed_in" : "kosync.signed_out");
    kosyncStatus(kosyncClient->storageError().isEmpty() ? status : kosyncClient->storageError());
    kosyncFinishingElapsed.invalidate();
    kosyncDeadline.setSingleShot(true);
    connect(&kosyncDeadline, &QTimer::timeout, this, [this] {
        ++kosyncEpoch;
        kosyncClient->cancel();
        kosyncFlushWaiting = false;
        finishKosync(strings::get("kosync.saved_locally"));
    });
    kosyncOpeningGrace.setSingleShot(true);
    connect(&kosyncOpeningGrace, &QTimer::timeout, this, [this] {
        if (kosyncOpening) showKosyncOpening();
        finishKosyncPresentation();
    });
    kosyncTimer.setParent(this);
    kosyncTimer.setObjectName("rrKosyncTimer");
    kosyncTimer.setInterval(30000);
    connect(&kosyncTimer, &QTimer::timeout, this, [this] { synchronizeKosync(); });
    kosyncTimer.start();
}
void Reader::prepareKosyncPresentation(std::function<void(bool)> finished) {
    if (kosyncStarted || book->usesNativeCloudSync() || !kosyncClient->signedIn() || !kosyncClient->automatic() ||
        book->pendingProgress() || book->deviceTransition() || model->pages() <= 0 || model->temporaryTypography()) {
        finished(true);
        return;
    }
    // Keep the host's loading screen for a fast exchange. The same request
    // continues behind a cancellable popover once the reader is presented.
    kosyncPresentationReady = std::move(finished);
    kosyncStarted = true;
    kosyncOpening = true;
    kosyncOpeningGrace.start(500);
    kosyncDeadline.start(10000);
    startKosyncExchange(false);
}
void Reader::finishKosyncPresentation() {
    kosyncOpeningGrace.stop();
    auto finished = std::exchange(kosyncPresentationReady, {});
    if (finished) finished(!closing && !lifecycleHidden);
}
void Reader::synchronizeKosync(bool interactive) {
    if (book->usesNativeCloudSync()) {
        kosyncStarted = true;
        if (interactive) kosyncStatus(strings::get("kosync.native_sync"));
        return;
    }
    if (kosyncProgress.pending()) {
        if (!interactive && !kosyncClient->automatic()) return;
        if (interactive) {
            kosyncPositionFailed = false;
            kosyncStatus(strings::get("kosync.pending"));
        }
        kosyncPromptDeferred = false;
        showKosyncChoice();
        return;
    }
    if (!kosyncClient->signedIn() || (!interactive && !kosyncClient->automatic()) || kosyncClient->busy() ||
        kosyncDialog || closing || lifecycleHidden || exportRunning ||
        services.suspended() || book->deviceTransition() || book->pendingProgress() || model->busy() || model->temporaryTypography() ||
        !isVisible() || model->pages() <= 0 || (!interactive && topControls->isVisible())) return;
    const bool opening = !kosyncStarted && !interactive;
    if (!interactive && !opening && !kosyncProgress.needsSync(model->state().value("position").toString())) return;
    kosyncStarted = true;
    kosyncDeadline.start(10000);
    if (opening) showKosyncOpening();
    startKosyncExchange(false);
}
void Reader::hideKosyncOpening() {
    if (!kosyncOpening) return;
    kosyncOpening = false;
    if (kosyncDialog) {
        kosyncDialog->hide();
        kosyncDialog->deleteLater();
        kosyncDialog = nullptr;
    }
}
void Reader::showKosyncOpening() {
    auto *dialog = new ui::NoticeModal(this,
        {strings::get("kosync.opening_description"), strings::get("kosync.opening"), {}},
        {strings::get("kosync.cancel_sync"), "rrKosyncCancel"});
    auto *card = dialog->card();
    auto *title = card->findChild<QLabel *>("rrModalTitle");
    const int index = card->rows()->indexOf(title);
    card->rows()->removeWidget(title);
    auto *heading = new ui::Panel(card);
    dialog->descriptionLabel()->setAlignment(Qt::AlignCenter);
    auto *row = new QHBoxLayout(heading);
    const auto padding = ui::metrics::notice::cardMargins;
    row->setContentsMargins(padding.left(), padding.top(), padding.right(), 0);
    row->setSpacing(ui::px(20));
    row->addStretch(1);
    auto *icon = new QLabel(heading);
    icon->setObjectName("rrKosyncStatusIcon");
    const int size = ui::px(48);
    icon->setFixedSize(size, size);
    icon->setPixmap(ui::controlIcon(ui::ControlIcon::Sync, ui::darkTheme(this) ? QColor(Qt::white) : QColor(Qt::black)).pixmap(size, size));
    row->addWidget(icon, 0, Qt::AlignVCenter);
    // The heading and description share one content block above the action.
    title->setProperty("reader_modal_content", false);
    title->setProperty("reader_modal_heading", false);
    title->style()->unpolish(title);
    title->style()->polish(title);
    title->setMinimumHeight(0);
    title->setMaximumHeight(QWIDGETSIZE_MAX);
    row->addWidget(title);
    row->addStretch(1);
    card->rows()->insertWidget(index, heading);
    auto *cancel = dialog->primaryButton();
    cancel->setProperty("reader_primary_action", false);
    ui::readerStyle(cancel, "QPushButton{border:none;padding:0;min-height:0;font-weight:400;" +
                            ui::styles::font(ui::metrics::chrome::labelText) + "}");
    kosyncOpening = true;
    kosyncDialog = dialog;
    dialog->setObjectName("rrKosyncOpening");
    const auto skip = [this] {
        cancelKosync();
        kosyncStatus(strings::get("kosync.skipped"));
        refresh();
    };
    connect(dialog->primaryButton(), &QPushButton::clicked, this, skip);
    dialog->dismissed = skip;
    dialog->show();
    dialog->raise();
    dialog->fitCard();
    refresh();
}
void Reader::cancelKosync() {
    ++kosyncEpoch;
    kosyncClient->cancel();
    kosyncDeadline.stop();
    kosyncOpeningGrace.stop();
    kosyncPresentationReady = {};
    kosyncFinished = {};
    kosyncFinishing = false;
    kosyncFlushWaiting = false;
    hideKosyncOpening();
}
void Reader::finishKosync(const QString &message) {
    if (kosyncFlushWaiting) {
        kosyncFlushWaiting = false;
        startKosyncExchange(true);
        return;
    }
    kosyncPage->setLastSync(kosyncClient->lastSync());
    const bool finishing = kosyncFinishing;
    const bool opening = kosyncOpening;
    hideKosyncOpening();
    kosyncDeadline.stop();
    kosyncFinishing = false;
    auto finished = std::move(kosyncFinished);
    kosyncFinished = {};
    kosyncMessage = message;
    kosyncPage->setStatus(message, false);
    if (!finishing && !closing && !lifecycleHidden && !exportRunning) {
        if (opening && message != strings::get("kosync.up_to_date") && message != strings::get("kosync.pending"))
            page->notice = message;
        refresh();
        showKosyncChoice();
    }
    finishKosyncPresentation();
    if (finished) finished();
}
void Reader::flushKosync(std::function<void()> finished) {
    // A local checkpoint is already durable. Final uploads use only an existing
    // connection and never resolve a remote conflict while closing or asleep.
    if (book->usesNativeCloudSync() || !kosyncClient->signedIn() || !kosyncClient->automatic() ||
        kosyncProgress.pending() || book->pendingProgress() || model->temporaryTypography() ||
        model->pages() <= 0 || kosyncDocument().isEmpty() ||
        model->state().value("position").toString().isEmpty()) {
        cancelKosync();
        finished();
        return;
    }
    hideKosyncOpening();
    kosyncFinishingElapsed.start();
    kosyncFinishing = true;
    kosyncFinished = std::move(finished);
    kosyncDeadline.start(kosyncFinishBudget);
    // Let an older PUT finish before sending the final position. Aborting its
    // reply cannot guarantee that the server has stopped processing the write.
    kosyncFlushWaiting = kosyncClient->busy();
    if (!kosyncFlushWaiting) startKosyncExchange(true);
}
void Reader::startKosyncExchange(bool finishing) {
    const auto document = kosyncDocument();
    if (document.isEmpty()) {
        finishKosync(strings::get(kosyncClient->filenameMatching() ? "kosync.filename_unavailable" : "kosync.unavailable"));
        return;
    }
    const auto epoch = kosyncEpoch;
    kosync::Progress snapshot;
    snapshot.position = model->state().value("position").toString();
    snapshot.percentage = model->pages() > 1 ? double(model->page()) / (model->pages() - 1) : 0;
    // Fetch before every write, including the bounded final attempt. A failed
    // fetch cannot authorize an overwrite of another device's progress.
    kosyncClient->pull(document, [this, document, epoch, finishing, snapshot](kosync::Progress remote, const QString &error) {
        if (epoch != kosyncEpoch) return;
        if (!error.isEmpty()) { finishKosync(strings::get("kosync.saved_locally")); return; }
        if (book->pendingProgress() || book->deviceTransition() ||
            (!finishing && (closing || lifecycleHidden || services.suspended() || model->busy() || exportRunning ||
                            (!isVisible() && !kosyncOpening)))) {
            finishKosync(strings::get("kosync.retry_later"));
            return;
        }
        auto local = snapshot;
        if (!finishing) {
            local.position = model->state().value("position").toString();
            local.percentage = model->pages() > 1 ? double(model->page()) / (model->pages() - 1) : 0;
        }
        const auto action = kosyncProgress.observe(remote, local.position, kosyncClient->deviceId());
        if (action == kosync::Reconciler::Action::Choose) { finishKosync(strings::get("kosync.pending")); return; }
        const bool renamed = remote.deviceId == kosyncClient->deviceId() &&
                             remote.device != kosyncClient->deviceName() && !local.position.isEmpty();
        if (action == kosync::Reconciler::Action::Current && !renamed) {
            finishKosync(strings::get(kosyncClient->recordSyncSuccess() ? "kosync.up_to_date" : "kosync.storage_error"));
            return;
        }
        kosyncClient->push(document, local, [this, epoch, local](const QString &pushError) {
            if (epoch != kosyncEpoch) return;
            if (pushError.isEmpty()) kosyncProgress.uploaded(local.position);
            const auto status = !pushError.isEmpty() ? "kosync.saved_locally"
                : kosyncClient->recordSyncSuccess() ? "kosync.up_to_date" : "kosync.storage_error";
            finishKosync(strings::get(status));
        });
        if (kosyncClient->busy()) kosyncStatus(strings::get("kosync.syncing"));
    }, !finishing && kosyncOpening);
    if (kosyncClient->busy()) kosyncStatus(strings::get("kosync.syncing"));
}
void Reader::showKosyncChoice() {
    if (!kosyncProgress.pending() || kosyncPositionFailed || kosyncPromptDeferred || kosyncDialog || closing || lifecycleHidden || !isVisible() || model->busy() ||
        services.suspended() || book->deviceTransition() || book->pendingProgress() || syncDialog || finishDialog ||
        topControls->isVisible() || general->isVisible() || annotationMode || searchPanel->isVisible() ||
        dictionaryPanel->isVisible() || linksActive() || lookupPending || appearanceEdit.panelOpen()) return;
    const auto remote = kosyncProgress.candidate();
    auto *dialog = new ui::ChoiceModal(this,
        {strings::get("kosync.comparing_positions"),
         strings::get("kosync.resume_title"), {}},
        {strings::get("kosync.keep_local"), "rrKosyncKeep"},
        {strings::get("kosync.use_remote"), "rrKosyncResume"});
    kosyncDialog = dialog;
    dialog->setObjectName("rrKosyncChoice");
    dialog->setDimmed(true);
    const auto dismiss = [this, dialog] {
        dialog->hide();
        dialog->deleteLater();
        kosyncDialog = nullptr;
    };
    const auto keep = [this, dismiss] {
        kosyncProgress.keepLocal();
        kosyncPositionFailed = false;
        dismiss();
        kosyncStatus(strings::get("kosync.kept_local"));
    };
    connect(dialog->secondaryButton(), &QPushButton::clicked, this, keep);
    // Closing the choice does not authorize a write over the remote position.
    dialog->dismissed = [this, dismiss] {
        kosyncPromptDeferred = true;
        dismiss();
    };
    connect(dialog->primaryButton(), &QPushButton::clicked, this, [this, remote, dismiss] {
        if (model->busy() || closing || lifecycleHidden || services.suspended()) return;
        dismiss();
        if (!model->goToSyncedPosition(remote.position, [this](const QString &error) {
            if (error.isEmpty()) {
                kosyncProgress.acceptRemote(model->state().value("position").toString());
                save(true);
                if (!kosyncClient->recordSyncSuccess()) {
                    kosyncStatus(strings::get("kosync.storage_error"));
                    refresh();
                    return;
                }
            } else {
                // Do not offer the same invalid position indefinitely. Keep
                // writes blocked until the user explicitly keeps local progress.
                kosyncPositionFailed = true;
                kosyncClient->setAutomatic(false);
                kosyncPage->setAccount(kosyncClient->server(), kosyncClient->username(), true, kosyncClient->automatic());
            }
            page->notice = error.isEmpty() ? strings::get("kosync.resumed") : error;
            kosyncStatus(page->notice);
            refresh();
        })) kosyncStatus(strings::get("kosync.retry_later"));
    });
    dialog->setGeometry(rect());
    dialog->show();
    dialog->raise();
    dialog->fitCard();
    const auto epoch = kosyncEpoch;
    const auto localChapter = model->chapterTitle();
    const double localPercentage = model->pages() > 1 ? 100.0 * model->page() / (model->pages() - 1) : 0;
    dialog->primaryButton()->setEnabled(false);
    QPointer<ui::ChoiceModal> guard(dialog);
    model->describeSyncedPosition(remote.position, [this, guard, epoch, remote, localChapter, localPercentage](SyncedPositionDescription info) {
        if (!guard || !guard->isVisible() || epoch != kosyncEpoch) return;
        const bool compared = info.comparison.status == PositionComparison::Status::Compared;
        const bool backward = compared && info.comparison.direction < 0;
        const auto direction = strings::get(!compared ? "kosync.direction_unknown" : backward ? "kosync.direction_backward" :
                                           info.comparison.direction > 0 ? "kosync.direction_forward" : "kosync.direction_same");
        auto *description = guard->descriptionLabel();
        const int width = guard->card()->width() - 2 * int(ui::metrics::notice::cardMargins.left()) - 4;
        const auto chapter = [&](QString value) {
            if (value.trimmed().isEmpty()) value = strings::get("kosync.chapter_unavailable");
            return ui::elideInterfaceText(value.simplified(), description->font(), width);
        };
        const auto device = remote.device.trimmed().isEmpty() ? strings::get("kosync.device_unknown") : remote.device.simplified();
        const auto timestamp = remote.timestamp > 0 ? QDateTime::fromMSecsSinceEpoch(remote.timestamp * 1000).toLocalTime() : QDateTime();
        const auto synced = timestamp.isValid()
            ? strings::get("kosync.synced_at").arg(QLocale().toString(timestamp, QLocale::ShortFormat))
            : strings::get("kosync.time_unavailable");
        description->setText(strings::get("kosync.position_comparison")
            .arg(QLocale().toString(localPercentage, 'f', 1), chapter(localChapter),
                 QLocale().toString(remote.percentage * 100, 'f', 1), chapter(info.chapter),
                 ui::elideInterfaceText(device, description->font(), width), synced, direction));
        guard->findChild<QLabel *>("rrModalTitle")->setText(strings::get(backward ? "kosync.resume_backward_title" : "kosync.resume_title"));
        guard->primaryButton()->setEnabled(true);
        if (backward) guard->secondaryButton()->setFocus(Qt::OtherFocusReason);
        guard->fitCard();
        refresh();
    });
}
}
