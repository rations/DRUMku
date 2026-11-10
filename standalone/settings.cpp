// Settings implementation. See settings.h.

#include "settings.h"

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>

namespace DRUMku
{

namespace
{

// A file holding much more than a plug-in state and a few strings is not ours.
const off_t kMaxSettingsSize = 64 * 1024 * 1024;

status_t SettingsPath(BPath &path, bool create)
{
    status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path, create);
    if (status != B_OK)
        return status;
    if ((status = path.Append("DRUMku")) != B_OK)
        return status;
    if (create && (status = create_directory(path.Path(), 0755)) != B_OK)
        return status;
    return path.Append("standalone");
}

} // namespace

BMessage LoadSettings()
{
    BMessage settings;
    BPath path;
    if (SettingsPath(path, false) != B_OK)
        return settings;
    BFile file(path.Path(), B_READ_ONLY);
    off_t size = 0;
    if (file.InitCheck() != B_OK || file.GetSize(&size) != B_OK || size <= 0 ||
        size > kMaxSettingsSize)
        return settings;
    if (settings.Unflatten(&file) != B_OK)
        settings.MakeEmpty();
    return settings;
}

status_t SaveSettings(const BMessage &settings)
{
    BPath path;
    status_t status = SettingsPath(path, true);
    if (status != B_OK)
        return status;
    BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    if ((status = file.InitCheck()) != B_OK)
        return status;
    return settings.Flatten(&file);
}

} // namespace DRUMku
