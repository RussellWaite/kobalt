#pragma once
#include <QObject>
#include <QPointer>
#include <functional>
class QLineEdit;
class QWidget;
enum KeyboardScript : int;
namespace kobalt {
extern void (*nativeKeyboardRepaint)(QWidget *, bool);
void styleNativeKeyboard(QWidget *frame);

// Used only after firmwareSupported() accepts Nickel.
class NativeKeyboard : public QObject {
    Q_OBJECT
public:
    // An editor's reader_keyboard_submit_text property overrides the Search key caption.
    NativeKeyboard(QWidget *parent, QLineEdit *editor);
    ~NativeKeyboard() override;
    bool show();
    void hide();
    void place();
    std::function<void()> submitted;

protected:
    bool eventFilter(QObject *, QEvent *) override;
private slots:
    void commit();
    void changeScript(KeyboardScript script);

private:
    QPointer<QWidget> frame;
    QPointer<QLineEdit> editor;
    QWidget *host;
    int script = 0;
    bool ready = false;
    bool create();
};
}
