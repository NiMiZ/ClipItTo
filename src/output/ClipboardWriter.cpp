#include "ClipboardWriter.h"

#include "ClipFilter.h"
#include "DesktopIntegration.h"

#include <QClipboard>

namespace clipitto {

ClipboardWriter::ClipboardWriter(QClipboard* clipboard, ClipFilter& filter, DesktopIntegration* integration)
    : m_clipboard(clipboard)
    , m_filter(filter)
    , m_integration(integration)
{
}

void ClipboardWriter::setText(const QString& text)
{
    m_filter.noteOwnWrite(text);
    if (m_integration && m_integration->isAvailable()) {
        m_integration->setClipboard(text);
    } else {
        m_clipboard->setText(text, QClipboard::Clipboard);
    }
}

} // namespace clipitto
