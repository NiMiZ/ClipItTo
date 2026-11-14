#pragma once

#include <QByteArray>

namespace clipitto::phone {

// The page the phone opens: one self-contained HTML document (its own style and script, no external resource, no address
// of any other site, no font, no image file). It sends pictures to "upload", typed text to "text" and asks "status" about
// each, all relative to the page's own address.
QByteArray pageHtml();

} // namespace clipitto::phone
