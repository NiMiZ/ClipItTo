#include "ClipboardWriter.h"

#include "ClipFilter.h"

#include <QClipboard>

namespace clipitto {

ClipboardWriter::ClipboardWriter(QClipboard* clipboard, ClipFilter& filter)
    : m_clipboard(clipboard)
    , m_filter(filter)
{
}

void ClipboardWriter::setText(const QString& text)
{
    m_filter.noteOwnWrite(text);
    m_clipboard->setText(text, QClipboard::Clipboard);
}

} // namespace clipitto
