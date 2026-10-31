// A binary glTF written by fastgltf's exporter, built without _UCRT (CMakeLists.txt), as
// cook/glb.hpp says.

#include "cook/glb.hpp"

#include <cstdint>
#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <format>
#include <optional>
#include <vector>

namespace fbe::tools {

std::expected<bytes_t, std::string> write_glb(const fastgltf::Asset& asset,
                                              std::span<const float> rates) {
    fastgltf::Exporter exporter;
    exporter.setUserPointer(&rates);
    exporter.setExtrasWriteCallback([](std::size_t index, fastgltf::Category category,
                                       void* user) -> std::optional<std::string> {
        const auto& given = *static_cast<const std::span<const float>*>(user);
        if (category != fastgltf::Category::Animations || index >= given.size() ||
            given[index] <= 0) {
            return std::nullopt;
        }
        return std::format(R"({{"frameRate":{}}})", given[index]);
    });
    auto written = exporter.writeGltfBinary(asset);
    if (written.error() != fastgltf::Error::None) {
        return std::unexpected(std::format("couldn't write it as a binary glTF: {}",
                                           fastgltf::getErrorMessage(written.error())));
    }
    const std::vector<std::byte>& output = written.get().output;
    const auto* start = reinterpret_cast<const std::uint8_t*>(output.data());
    return bytes_t(start, start + output.size());
}

} // namespace fbe::tools
