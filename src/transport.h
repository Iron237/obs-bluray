#pragma once
#include <cstdint>
#include <cstddef>
namespace sd {
// Convert clip-local MPEG-TS timestamps to one 90 kHz playlist timeline.
// Only timestamp fields are changed; compressed payloads remain intact.
void rebaseM2ts(std::uint8_t *data,std::size_t size,std::int64_t offset90k);
}
