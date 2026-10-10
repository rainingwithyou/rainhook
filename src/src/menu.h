#pragma once
// ImGui menu.
//
// Layout is a left sidebar of sections (Visual / Combat / Map / Status /
// Settings) with a scrolling content pane, which is the shape the reference mod
// uses and the only one that stays navigable at this option count.

namespace menu {
bool Visible();
void Draw();          // full menu window, when Visible()
void DrawLauncher();  // floating button used to open the menu
void DrawWatermark();

// Floating in-match toggles (the Kimmy "KM" button). Drawn last each frame so
// nothing overlaps their hit boxes.
void DrawFloatingButtons();
}
