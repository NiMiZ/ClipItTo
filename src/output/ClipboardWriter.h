#pragma once

#include <QString>

class QClipboard;

namespace clipitto {

class ClipFilter;

// Puts a history entry back on the system clipboard. The capture filter is told first,
// so the resulting clipboard-change echo is not recorded as a new copy.
class ClipboardWriter {
public:
    ClipboardWriter(QClipboard* clipboard, ClipFilter& filter);

    // On Wayland this must run while one of our windows has keyboard focus.
    void setText(const QString& text);

private:
    QClipboard* m_clipboard;
    ClipFilter& m_filter;
};

} // namespace clipitto
