#pragma once

#include "SunPosition.hpp"

// The place on Earth and the day, as the panels that set the sun edit them (the
// Advanced nature panel for the scene, the world generator for the world it
// makes). One widget so the two cannot drift apart. Editor-only.
namespace sunui {

// A city to start from, latitude and longitude, the time zone and its summer
// time, and the date -- then a line saying where the sun stands at clock time
// `hours` and when it rises and sets. True when anything changed. Any change
// takes a legacy scene's sun off its old arc (sunpos::Place::legacy).
bool placeEditor(sunpos::Place& p, float hours);

// "13:27" for clock hours (any value, wrapped into the day).
const char* clock(float hours);

} // namespace sunui
