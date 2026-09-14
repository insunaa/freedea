#pragma once

#include <FreeInkUICore.h>

// DisplayTarget owns eight font slots. Slots 0..2 are the shared small/body/title
// slots; slot 3 is a compact Noto Sans variant used where long status text must
// fit in a constrained row without forcing the whole UI to a smaller font.
namespace freedea {
inline constexpr freeink::ui::FontId kFontSlotCompact = 3;
} // namespace freedea
