// Settings — what the standalone app remembers between runs, kept as a
// flattened BMessage in ~/config/settings/DRUMku/standalone:
//   "backend"         string  "mediakit" or "jack"
//   "midi:mediakit"   strings the MIDI sources last connected on the Media Kit
//   "midi:jack"       strings the MIDI sources last connected on JACK
//                     (absent: none chosen yet, so every source is connected)
//   "frame"           BRect   the window's frame
//   "component"       B_RAW_TYPE  the plug-in's processor state
//   "controller"      B_RAW_TYPE  the plug-in's controller state
// The file is read once at start and written when the window closes.

#pragma once

#include <Message.h>

namespace DRUMku
{

// The settings file's contents; an empty message if it is missing or unreadable.
BMessage LoadSettings();
// Writes the settings file (directories created as needed).
status_t SaveSettings(const BMessage &settings);

} // namespace DRUMku
