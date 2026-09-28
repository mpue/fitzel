#pragma once

#include <filesystem>
#include <string>

namespace fitzel { struct ImagePixels; }

// The Assets browser's thumbnail disk cache (see ThumbCache.cpp). Editor only.
namespace thumbcache {

std::filesystem::path thumbCacheDir();
long long sourceMtime(const std::string& path);
// False when there is no valid entry for this source as it is now.
bool loadThumbCache(const std::filesystem::path& file, long long srcMtime,
                    fitzel::ImagePixels& out);
void saveThumbCache(const std::filesystem::path& file, long long srcMtime,
                    const fitzel::ImagePixels& img);

} // namespace thumbcache
