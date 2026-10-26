#include "support/qt_compat.h"
#include "platforms/kobo/common/native_keyboard.h"
#include "platform/api/kobo_refresh.h"
#include "support/diagnostics.h"
#include <QApplication>
#include <QGraphicsEffect>
#include <QLineEdit>
#include <QLocale>
#include <QMetaType>
#include <QEvent>
#include <QTimer>
#include <QVariant>
#include <dlfcn.h>

namespace kobalt {
namespace {
template <class T> T symbol(const char *name) {
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}
struct Api {
    void (*frame)(void *, QWidget *) = symbol<decltype(frame)>("_ZN13KeyboardFrameC1EP7QWidget");
    void (*destroyFrame)(QWidget *) = symbol<decltype(destroyFrame)>("_ZN13KeyboardFrameD0Ev");
    QObject *(*create)(QWidget *, int, const QLocale &) =
        symbol<decltype(create)>("_ZN13KeyboardFrame14createKeyboardE14KeyboardScriptRK7QLocale");
    void (*receiver)(void *, QLineEdit *, bool) = symbol<decltype(receiver)>("_ZN16KeyboardReceiverC1EP9QLineEditb");
    void (*attach)(QObject *, QObject *,
                   bool) = symbol<decltype(attach)>("_ZN24SearchKeyboardController11setReceiverEP16KeyboardReceiverb");
    void (*goText)(QObject *,
                   const QString &) = symbol<decltype(goText)>("_ZN24SearchKeyboardController9setGoTextERK7QString");
    QMetaType frameType{symbol<const QtPrivate::QMetaTypeInterface *>(
        "_ZN9QtPrivate25QMetaTypeInterfaceWrapperI13KeyboardFrameE8metaTypeE")};
    QMetaType receiverType{symbol<const QtPrivate::QMetaTypeInterface *>(
        "_ZN9QtPrivate25QMetaTypeInterfaceWrapperI16KeyboardReceiverE8metaTypeE")};
    bool valid() const {
        return frame && destroyFrame && create && receiver && attach && goText && frameType.sizeOf() == 40 &&
               receiverType.sizeOf() == 36;
    }
};
const Api &api() {
    static const Api value;
    return value;
}
}
NativeKeyboard::NativeKeyboard(QWidget *parent, QLineEdit *editor) : QObject(parent), editor(editor), host(parent) {
    editor->installEventFilter(this);
}
NativeKeyboard::~NativeKeyboard() {
    if (frame)
        api().destroyFrame(frame);
}
bool NativeKeyboard::create() {
    if (!api().valid() || !editor)
        return false;
    if (!frame) {
        // 5.18 exports both allocation sizes in its Qt metatypes. The native
        // deleting destructor releases the frame and its QObject children.
        void *storage = ::operator new(size_t(api().frameType.sizeOf()));
        try {
            api().frame(storage, host);
        } catch (...) {
            ::operator delete(storage);
            return false;
        }
        frame = static_cast<QWidget *>(storage);
        frame->setObjectName("rrSearchKeyboard");
        frame->setPalette(QApplication::palette());
        frame->setStyleSheet("QWidget{background:white;color:black;}");
        styleNativeKeyboard(frame);
    }
    auto *controller = api().create(frame, script, QLocale());
    if (!controller)
        return false;
    // AbstractController reparents itself to the VirtualKeyboard view when
    // loading it. That view remains a child of our KeyboardFrame.
    auto *owner = qobject_cast<QWidget *>(controller->parent());
    if (!owner || (owner != frame && !frame->isAncestorOf(owner)))
        return false;
    void *storage = ::operator new(size_t(api().receiverType.sizeOf()));
    try {
        api().receiver(storage, editor, false);
    } catch (...) {
        ::operator delete(storage);
        return false;
    }
    // LibraryFinderController uses this same sequence. The receiver constructor
    // parents it to the editor; setReceiver retains a guarded QObject pointer.
    api().attach(controller, static_cast<QObject *>(storage), true);
    const auto submitText = editor->property("reader_keyboard_submit_text").toString();
    api().goText(controller, submitText.isEmpty() ? QStringLiteral("Search") : submitText);
    connect(controller, SIGNAL(commitRequested()), this, SLOT(commit()));
    connect(controller, SIGNAL(returnPressed()), this, SLOT(commit()));
    connect(controller, SIGNAL(languageChanged(KeyboardScript)), this, SLOT(changeScript(KeyboardScript)));
    for (auto *widget : frame->findChildren<QWidget *>())
        platform::textRefresh(widget);
    platform::textRefresh(frame);
    ready = true;
    return true;
}
bool NativeKeyboard::show() {
    if (!ready && !create()) {
        diagnostic("search-keyboard-unavailable");
        return false;
    }
    const bool opening = !frame->isVisible();
    place();
    if (opening) {
        frame->show();
        frame->raise();
    }
    host->window()->setProperty("reader_keyboard_bounds", QRect(frame->mapTo(host->window(), QPoint()), frame->size()));
    // A queued focus event can call show again after the first frame was
    // painted. Only opening the keyboard needs its theme effect redrawn.
    if (opening && frame->graphicsEffect())
        frame->graphicsEffect()->update();
    editor->setFocus();
    return true;
}
void NativeKeyboard::place() {
    if (!frame)
        return;
    frame->resize(host->width(), qMax(1, frame->sizeHint().height()));
    const int height = qMax(1, frame->sizeHint().height());
    frame->setGeometry(0, host->height() - height, host->width(), height);
    host->window()->setProperty("reader_keyboard_bounds",
                                frame->isVisible() ? QRect(frame->mapTo(host->window(), QPoint()), frame->size())
                                                   : QRect());
}
void NativeKeyboard::hide() {
    host->window()->setProperty("reader_keyboard_bounds", QRect());
    if (frame)
        frame->hide();
}
void NativeKeyboard::commit() {
    // A physical Return can also reach QLineEdit directly. Search hides the
    // keyboard, so the controller's follow-up signal must not submit twice.
    if (frame && frame->isVisible() && host->isVisible() && submitted)
        submitted();
}
void NativeKeyboard::changeScript(KeyboardScript value) {
    script = int(value);
    kobalt::qt::later(0, this, [this] {
        if (host->isVisible() && create())
            show();
    });
}
bool NativeKeyboard::eventFilter(QObject *object, QEvent *event) {
    if (object == editor && (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonRelease))
        kobalt::qt::later(0, this, [this] {
            if (host->isVisible() && editor && editor->hasFocus())
                show();
        });
    return QObject::eventFilter(object, event);
}
}
