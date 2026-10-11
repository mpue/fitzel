#pragma once

// The "Log" window: what the process printed to stdout and stderr, as captured
// by fitzel::logcap (engine/core/LogCapture.hpp). A filter, a toggle per stream,
// auto-scroll, Clear and Copy; lines that read like an error or a warning are
// coloured so they can be found without reading everything else.
//
// The panel keeps its own copy of the lines and pulls only the new ones each
// frame, so an open log costs a lock and a few appends, not a copy of the ring.
// Editor only.
namespace logui {

struct PanelState {
    bool& show;
};

void drawPanel(const PanelState& s);

} // namespace logui
