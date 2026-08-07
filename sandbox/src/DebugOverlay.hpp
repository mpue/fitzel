#pragma once

// The "Performance" window: what the frame costs, and where.
//
// Reads prof:: for CPU timings and asks the OS and the GL driver for memory. Its
// reason for existing is diagnosing hitches, so the numbers it leads with are
// the worst frame in the recent window and the 1% low -- not average fps, which
// is the one number that reliably hides a stutter.
namespace debugoverlay {

// Draw the window. `open` is the View-menu toggle; the window's own close box
// clears it. Call between gui.beginFrame() and gui.endFrame().
void draw(bool* open);

} // namespace debugoverlay
