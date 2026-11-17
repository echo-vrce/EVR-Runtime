// The main menu's UPDATES card and its panel (PC first; the logic headers are shared with Quest).
#pragma once

namespace evr_updates {

/// Places the UPDATES card, then shows the loader's update offer on it and in its panel. Call after the friends panel
/// installs (its level edits run first) and before the event hook.
void Install();

}  // namespace evr_updates
