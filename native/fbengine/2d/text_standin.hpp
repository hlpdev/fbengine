#pragma once

// Until the input lane's text core is on master, a stand-in for its atlas and lay_out, so the
// canvas's text is built and tested against text_core.hpp without fonts: its atlas keeps and trims
// pages as the core's does, and its lay_out is whatever's set, which this lane's tests set to lay
// out glyphs of their own. It goes when the core lands.

#include "2d/text_core.hpp"

#include <cstdint>
#include <string_view>

namespace fbe::text {

/// What lays text out as lay_out does.
using lay_out_t = bool (*)(std::uint64_t font, std::string_view text, float scale, layout_t& into);

/// Sets what lay_out lays text out with; without one, it fails.
void set_lay_out(lay_out_t laying);

} // namespace fbe::text
