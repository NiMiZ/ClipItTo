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
    // A password copy: the text with the markers password managers set, so other clipboard
    // managers (and Windows' clipboard history) skip it too. Through the GNOME extension the
    // markers can't be added (the shell takes one format per write); the text goes alone.
    void setPassword(const QString& text);
    // A copy with its formatted (HTML) version too. Always through the Qt clipboard: the GNOME extension
    // takes one format per write, so it can't carry both. (Without an integration on Wayland this needs focus, as
    // for every write; the popup still has it when a copy is chosen.)
    void setRich(const QString& text, const QString& html);
    // Empties the clipboard (a pasted password's time is up).
    void clear();
    // A picture (PNG) from the history; `pixelHash` lets the capture filter ignore its echo.
    void setImage(const QByteArray& png, const QString& pixelHash);

private:
    QClipboard* m_clipboard;
    ClipFilter& m_filter;
    DesktopIntegration* m_integration;
};

} // namespace clipitto
