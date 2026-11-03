#pragma once
#include "redirector.h"

namespace batman_mods {

// Each backend supplies a distinct tag, preserving its own recursion scope.
// Names are borrowed only while the caller and its ExternalAsset stay alive.
template<class Backend>
class AssetSelection {
public:
    AssetSelection(const char* original, const char* resolved) noexcept
        : original_(original), resolved_(resolved), previous_(current_) { current_ = this; }
    ~AssetSelection() { current_ = previous_; }
    AssetSelection(const AssetSelection&) = delete;
    AssetSelection& operator=(const AssetSelection&) = delete;

    static bool Contains(const char* path) noexcept {
        for (auto selected = current_; selected; selected = selected->previous_)
            if (SameExternalAssetPath(path, selected->original_) ||
                SameExternalAssetPath(path, selected->resolved_)) return true;
        return false;
    }
private:
    const char* original_;
    const char* resolved_;
    const AssetSelection* previous_;
    inline static thread_local const AssetSelection* current_ = nullptr;
};

} // namespace batman_mods
