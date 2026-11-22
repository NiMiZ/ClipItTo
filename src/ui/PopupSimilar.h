#pragma once

#include "PopupWindow.h"
#include "Similar.h"

namespace clipitto::ui {

// The state of the "similar copies" view (PopupSimilar.cpp). Only the copies of the view are kept, nothing is logged.
struct PopupWindow::SimilarScan {
    explicit SimilarScan(const Entry& reference)
        : refId(reference.id)
        , ref(reference.content)
    {
    }
    qint64 refId;
    similar::Reference ref;
    std::vector<Entry> shown; // the reference first, then what was found (whole copies)
    std::vector<Entry> page; // read, not compared yet
    std::size_t pos = 0;
    int offset = 0; // text copies read so far
    int compared = 0;
    qint64 scanMs = 0; // time spent comparing, slices only
    bool finished = false;
    bool outOfTime = false;
    bool capped = false; // stopped at kMaxCopies
};

} // namespace clipitto::ui
