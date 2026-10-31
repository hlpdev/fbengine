// Running a load, and the pool of loads: a slot each, whose generation goes up as it's freed, and
// which is retired once its last generation is, so a handle is never another load's.

#include "assets/loads.hpp"

#include <format>
#include <utility>

namespace fbe::assets {

namespace {

constexpr std::uint32_t last_generation = (1U << 24U) - 1U;
// A model's progress after its file's read; the rest is decoding it.
constexpr float read_part = 0.1F;

} // namespace

void run_load(load_t& load, jobs_t* jobs) {
    result_t<bytes_t> bytes = vfs::files().read(load.path);
    if (!bytes) {
        load.error = std::move(bytes.error());
        load.progress = 1;
        return;
    }
    switch (load.kind) {
    case load_kind_t::BYTES:
        load.bytes = std::move(*bytes);
        break;
    case load_kind_t::IMAGE: {
        load.progress = 0.5F;
        result_t<image_t> image = decode_image(
            *bytes, { .srgb = !load.options.linear, .transcode = load.options.transcode },
            load.path.text());
        if (image) {
            load.image = std::move(*image);
        } else {
            load.error = std::move(image.error());
        }
        break;
    }
    case load_kind_t::MODEL: {
        load.progress = read_part;
        const model_options_t options{
            .transcode = load.options.transcode,
            .jobs = jobs,
            .progress =
                [&load](float done) { load.progress = read_part + ((1 - read_part) * done); },
        };
        result_t<model_t> model = decode_model(load.path, *bytes, vfs::files(), options);
        if (model) {
            load.model = std::move(*model);
        } else {
            load.error = std::move(model.error());
        }
        break;
    }
    }
    load.progress = 1;
}

std::uint64_t loads_t::add(std::shared_ptr<load_t> load) {
    std::uint32_t slot = 0;
    if (m_free.empty()) {
        slot = static_cast<std::uint32_t>(m_slots.size());
        m_slots.emplace_back();
    } else {
        slot = m_free.back();
        m_free.pop_back();
    }
    m_slots[slot].load = std::move(load);
    ++m_count;
    return (std::uint64_t{ LOAD_KIND } << 56U) |
           (static_cast<std::uint64_t>(m_slots[slot].generation) << 32U) | slot;
}

std::optional<std::uint32_t> loads_t::slot_of(std::uint64_t handle) const {
    const auto slot = static_cast<std::uint32_t>(handle);
    const auto generation = static_cast<std::uint32_t>(handle >> 32U) & last_generation;
    if ((handle >> 56U) != LOAD_KIND || slot >= m_slots.size() || generation == 0 ||
        m_slots[slot].generation != generation || m_slots[slot].load == nullptr) {
        return std::nullopt;
    }
    return slot;
}

load_t* loads_t::get(std::uint64_t handle) const {
    const std::optional<std::uint32_t> slot = slot_of(handle);
    return slot ? m_slots[*slot].load.get() : nullptr;
}

std::shared_ptr<load_t> loads_t::share(std::uint64_t handle) const {
    const std::optional<std::uint32_t> slot = slot_of(handle);
    return slot ? m_slots[*slot].load : nullptr;
}

bool loads_t::remove(std::uint64_t handle) {
    const std::optional<std::uint32_t> slot = slot_of(handle);
    if (!slot) {
        return false;
    }
    slot_t& freed = m_slots[*slot];
    freed.load = nullptr;
    --m_count;
    // A slot of the last generation is retired, so no handle comes round again.
    if (freed.generation == last_generation) {
        freed.generation = 0;
    } else {
        ++freed.generation;
        m_free.push_back(*slot);
    }
    return true;
}

std::size_t loads_t::count() const {
    return m_count;
}

loads_t& loads() {
    // Never destroyed, as the jobs aren't, which a load's work runs on.
    static auto* const engine = new loads_t();
    return *engine;
}

} // namespace fbe::assets
