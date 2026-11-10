#ifndef OPENMW_APPS_TES5VIEWER_PLUGINLIST_H
#define OPENMW_APPS_TES5VIEWER_PLUGINLIST_H

#include <filesystem>
#include <vector>

namespace TES5Viewer
{
    // UTF-8 plain active filenames or Skyrim plugins.txt (*active, unmarked disabled).
    // Present official masters are prefixed in canonical order; custom order is preserved.
    std::vector<std::filesystem::path> loadPluginList(
        const std::filesystem::path& list, const std::filesystem::path& data);
}

#endif
