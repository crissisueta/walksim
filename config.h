#pragma once

// Loads/saves the small human-readable settings.cfg in the project directory.
// Invalid entries are ignored so the already-initialized runtime defaults win.
void Config_Load(bool *panelOpen, bool *buildingDebug);
void Config_Save(bool panelOpen, bool buildingDebug);

// Draws the settings panel and handles its mouse input (sliders, reset button).
// Call once per frame while the panel is open, after the 3D drawing.
// Returns true if a terrain setting changed this frame, meaning the caller
// should rebuild the terrain mesh.
bool Config_Draw(void);
bool Config_ConsumeChanged(void); // true once after any UI setting changed
