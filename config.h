#pragma once

// Draws the settings panel and handles its mouse input (sliders, reset button).
// Call once per frame while the panel is open, after the 3D drawing.
// Returns true if a terrain setting changed this frame, meaning the caller
// should rebuild the terrain mesh.
bool Config_Draw(void);
