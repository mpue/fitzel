#pragma once

// How the editor's viewport draws the scene. Wireframe and the two Solids strip
// the picture down so shape reads on its own, Solid lit keeps the scene's light
// without the paintwork arguing with it, and Textured is the game. Editor only
// -- play mode always draws the game.
//
// Not saved: it is a way of LOOKING at the scene for a minute, not a property
// of it, and a project that reopened in wireframe because somebody once checked
// a normal would be a puzzle, not a convenience. Pathtraced is the odd one out:
// the other four are the raster renderer told to show less, this one is a
// different renderer altogether, running in the background and handing the
// viewport a picture (see ViewportTrace.hpp).
enum ViewShade { kShadeTextured = 0, kShadeSolid = 1, kShadeSolidLit = 2,
                 kShadeWireframe = 3, kShadePathTraced = 4 };
