#pragma once
#include <filesystem>

namespace dinero::storage {
inline constexpr const char* OrchardHandoffSelected=".orchard-handoff-selected-v1";
inline constexpr const char* OrchardHandoffCompleted=".orchard-handoff-selected-v1.done";
// Routing fence only, never state authority. Includes dangling links and
// ambiguous partials, and refuses unreadable paths. Check BEFORE directory
// creation or engine Open, including non-daemon maintenance callers.
inline bool OrchardHandoffControlsAbsent(const std::filesystem::path& parent){
    for(const auto* name:{OrchardHandoffSelected,".orchard-handoff-selected-v1.partial",
        OrchardHandoffCompleted,".orchard-handoff-selected-v1.done.partial"}){
        std::error_code error;const auto status=std::filesystem::symlink_status(parent/name,error);
        if(status.type()!=std::filesystem::file_type::not_found||
            (error&&error!=std::errc::no_such_file_or_directory))return false;
    }return true;
}
} // namespace dinero::storage
