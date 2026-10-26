#pragma once

#include <QFile>
#include <QFontDatabase>
#include <QRawFont>
#include <QSet>
#include <QVector>
#include "interfaces/reader/ui/style/private_text.h"
#include "interfaces/reader/ui/style/fonts.h"

namespace kobalt::reader {

inline void registerEditorFont(const QFont &font, QVector<int> &ids) {
    if (QRawFont::fromFont(ui::renewedFont(font)).familyName() == font.family())
        return;
    // Qt 5.2 keeps reading-font previews private to avoid duplicate font
    // loads. Load just the edited face when a native editor needs it.
    const auto bytes = ui::nativeEditorFontData(font);
    if (bytes.isEmpty())
        return;
    const int id = QFontDatabase::addApplicationFontFromData(bytes);
    if (id >= 0)
        ids.append(id);
}

inline void registerPickerFont(const QString &path, bool copyForQt, QSet<QString> &paths, QVector<int> &ids) {
    if (paths.contains(path))
        return;
    int id = -1;
    if (copyForQt) {
        // Qt can retain a face after removal. Read it into memory so USB
        // storage can unmount while the picker is open.
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) && file.size() <= 25 * 1024 * 1024)
            id = QFontDatabase::addApplicationFontFromData(file.readAll());
    } else {
        id = QFontDatabase::addApplicationFont(path);
    }
    if (id < 0)
        return;
    paths.insert(path);
    ids.append(id);
}

}
