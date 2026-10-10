#pragma once

#include <QString>

class QClipboard;

namespace clipitto {

class ClipFilter;
class DesktopIntegration;

// Puts a history entry back on the system clipboard. The capture filter is told first,
// so the resulting clipboard-change echo is not recorded as a new copy.
class ClipboardWriter {
public:
    // `integration` may be null. When available it owns the clipboard on our behalf, which
    // works without focus and survives daemon restarts.
    ClipboardWriter(QClipboard* clipboard, ClipFilter& filter, DesktopIntegration* integration = nullptr);

    // Without an integration on Wayland, this must run while one of our windows has focus.
    void setText(const QString& text);

private:
    QClipboard* m_clipboard;
    ClipFilter& m_filter;
    DesktopIntegration* m_integration;
};

} // namespace clipitto
