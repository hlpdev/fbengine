// The C API over the engine's loads: each a job, and its hand-over, which marks it done and gives
// the FB side its handle as the ticket.

#include "assets/loads.hpp"
#include "core/engine.hpp"
#include "fbengine.h"
#include "jobs/jobs.hpp"
#include "vfs/vfs.hpp"

#include <format>
#include <memory>

namespace {

namespace assets = fbe::assets;

int32_t failure_of(fbe::vfs::failure_t failure) {
    switch (failure) {
    case fbe::vfs::failure_t::NOT_FOUND:
        return FBE_ASSETS_NOT_FOUND;
    case fbe::vfs::failure_t::IS_A_FOLDER:
        return FBE_ASSETS_IS_A_FOLDER;
    case fbe::vfs::failure_t::NOT_A_FOLDER:
        return FBE_ASSETS_NOT_A_FOLDER;
    case fbe::vfs::failure_t::INVALID_PATH:
        return FBE_ASSETS_INVALID_PATH;
    case fbe::vfs::failure_t::INVALID_DATA:
        return FBE_ASSETS_INVALID_DATA;
    case fbe::vfs::failure_t::OTHER:
        break;
    }
    return FBE_ASSETS_OTHER;
}

// A new load of `path` as `kind`, or none, with fbe_error saying why.
std::shared_ptr<assets::load_t> made(int32_t kind, const char* path, bool linear) {
    if (kind < FBE_ASSETS_BYTES || kind > FBE_ASSETS_MODEL) {
        fbe::fail(std::format("a load's kind is FBE_ASSETS_BYTES, IMAGE, or MODEL, not {}", kind));
        return nullptr;
    }
    fbe::vfs::result_t<fbe::vfs::path_t> parsed = fbe::vfs::parse(path);
    if (!parsed) {
        fbe::fail(parsed.error().message);
        return nullptr;
    }
    auto load = std::make_shared<assets::load_t>();
    load->kind = static_cast<assets::load_kind_t>(kind);
    load->path = std::move(*parsed);
    load->options.linear = linear;
    return load;
}

// The load a handle is, if it's done and loaded as `kind`.
const assets::load_t* loaded(uint64_t handle, assets::load_kind_t kind) {
    const assets::load_t* load = assets::loads().get(handle);
    return load != nullptr && load->done && !load->error && load->kind == kind ? load : nullptr;
}

} // namespace

extern "C" {

FBE_API uint64_t fbe_assets_load(int32_t kind, const char* path, bool linear) {
    const std::shared_ptr<assets::load_t> load = made(kind, path, linear);
    if (load == nullptr) {
        return 0;
    }
    assets::run_load(*load, &fbe::jobs());
    load->done = true;
    return assets::loads().add(load);
}

FBE_API uint64_t fbe_assets_load_async(int32_t kind, const char* path, bool linear) {
    std::shared_ptr<assets::load_t> load = made(kind, path, linear);
    if (load == nullptr) {
        return 0;
    }
    const uint64_t handle = assets::loads().add(load);
    fbe::jobs_t& jobs = fbe::jobs();
    assets::load_t& started = *load;
    started.job = jobs.run([&jobs, load = std::move(load), handle] mutable {
        assets::run_load(*load, &jobs);
        // A load freed meanwhile is done all the same, and its ticket goes to no one.
        jobs.to_main([load = std::move(load), handle] {
            load->done = true;
            fbe::hand_to_fb(handle);
        });
    });
    return handle;
}

FBE_API bool fbe_assets_done(uint64_t load) {
    const assets::load_t* found = assets::loads().get(load);
    return found != nullptr && found->done;
}

FBE_API float fbe_assets_progress(uint64_t load) {
    const assets::load_t* found = assets::loads().get(load);
    return found != nullptr ? found->progress.load() : -1;
}

FBE_API bool fbe_assets_failed(uint64_t load) {
    const assets::load_t* found = assets::loads().get(load);
    return found != nullptr && found->done && found->error;
}

FBE_API int32_t fbe_assets_failure(uint64_t load) {
    const assets::load_t* found = assets::loads().get(load);
    return found != nullptr && found->done && found->error ? failure_of(found->error->failure) : -1;
}

FBE_API const char* fbe_assets_error(uint64_t load) {
    const assets::load_t* found = assets::loads().get(load);
    return found != nullptr && found->done && found->error ? found->error->message.c_str() : "";
}

FBE_API const char* fbe_assets_path(uint64_t load) {
    static thread_local std::string path;
    const assets::load_t* found = assets::loads().get(load);
    path = found != nullptr ? found->path.key() : "";
    return path.c_str();
}

FBE_API bool fbe_assets_has_settings(uint64_t load) {
    const assets::load_t* found = assets::loads().get(load);
    return found != nullptr && found->done && found->settings.has_value();
}

FBE_API const char* fbe_assets_settings(uint64_t load) {
    const assets::load_t* found = assets::loads().get(load);
    return found != nullptr && found->done && found->settings ? found->settings->c_str() : "";
}

FBE_API bool fbe_assets_wait(uint64_t load) {
    const assets::load_t* found = assets::loads().get(load);
    if (found == nullptr) {
        return false;
    }
    if (!found->done) {
        fbe::jobs().wait(found->job);
        // Its work for the main thread is queued now, and marks it done.
        fbe::hand_over_jobs();
    }
    return true;
}

FBE_API bool fbe_assets_free(uint64_t load) {
    return assets::loads().remove(load);
}

FBE_API int64_t fbe_assets_count(void) {
    return static_cast<int64_t>(assets::loads().count());
}

FBE_API int64_t fbe_assets_size(uint64_t load) {
    const assets::load_t* found = loaded(load, assets::load_kind_t::BYTES);
    return found != nullptr ? static_cast<int64_t>(found->bytes.size()) : -1;
}

FBE_API const uint8_t* fbe_assets_bytes(uint64_t load) {
    const assets::load_t* found = loaded(load, assets::load_kind_t::BYTES);
    return found != nullptr ? found->bytes.data() : nullptr;
}

FBE_API bool fbe_assets_image(uint64_t load, fbe_image_info_t* info) {
    const assets::load_t* found = loaded(load, assets::load_kind_t::IMAGE);
    if (found == nullptr) {
        return false;
    }
    const assets::image_t& image = found->image;
    *info = {
        .format = image.format,
        .width = image.width,
        .height = image.height,
        .depth = image.depth,
        .layers = image.layers,
        .faces = image.faces,
        .levels = static_cast<uint32_t>(image.levels.size()),
    };
    return true;
}

FBE_API bool fbe_assets_model(uint64_t load, fbe_model_info_t* info) {
    const assets::load_t* found = loaded(load, assets::load_kind_t::MODEL);
    if (found == nullptr) {
        return false;
    }
    const assets::model_t& model = found->model;
    int32_t primitives = 0;
    for (const assets::mesh_t& mesh : model.meshes) {
        primitives += static_cast<int32_t>(mesh.primitives.size());
    }
    *info = {
        .meshes = static_cast<int32_t>(model.meshes.size()),
        .primitives = primitives,
        .materials = static_cast<int32_t>(model.materials.size()),
        .images = static_cast<int32_t>(model.images.size()),
        .nodes = static_cast<int32_t>(model.nodes.size()),
        .scenes = static_cast<int32_t>(model.scenes.size()),
        .skins = static_cast<int32_t>(model.skins.size()),
        .clips = static_cast<int32_t>(model.clips.size()),
        .cameras = static_cast<int32_t>(model.cameras.size()),
        .lights = static_cast<int32_t>(model.lights.size()),
    };
    return true;
}

} // extern "C"
