#pragma once
#include <functional>
#include <memory>
class QWidget;
class QLineEdit;
class QString;

namespace kobalt {
// Destroy before the editor and panel. The host owns any native keyboard
// widgets; the reader owns this controller, not those widgets.
class KeyboardController {
public:
    virtual ~KeyboardController() = default;
    virtual bool show() = 0;
    virtual void hide() = 0;
    virtual void place() = 0;
    // Native keyboards can label their submit key before the first show.
    virtual void setSubmitText(const QString &) {}
    std::function<void()> submitted;
};
using KeyboardFactory = std::function<std::unique_ptr<KeyboardController>(QWidget *, QLineEdit *)>;
}
