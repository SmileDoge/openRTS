#include "shader_code.hpp"

#include <openrts/renderer/shader_directory.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace openrts::renderer {
namespace {

struct EmbeddedShader {
    std::string_view name;
    const char* text;
};

// shader_sources.inc (made by cmake/embed_shaders.cmake): `embedded_shaders`,
// every file of renderer/shaders by its name.
#include "shader_sources.inc"

struct ShaderFolder {
    std::mutex lock;
    bool chosen = false;
    std::optional<std::filesystem::path> folder;
    std::map<std::string, std::string, std::less<>> read; // name -> text, kept for the program's life
};

ShaderFolder& shader_folder() {
    static ShaderFolder folder;
    return folder;
}

// The folder, from the environment unless set; under the lock.
const std::optional<std::filesystem::path>& folder_in_use(ShaderFolder& state) {
    if (!state.chosen) {
        state.chosen = true;
#ifdef _WIN32
        // Wide: a folder named in Cyrillic, whatever the code page.
        wchar_t* value = nullptr;
        std::size_t length = 0;
        if (_wdupenv_s(&value, &length, L"OPENRTS_SHADER_DIR") == 0 && value && *value) state.folder = std::filesystem::path(value);
        std::free(value);
#else
        if (const char* value = std::getenv("OPENRTS_SHADER_DIR"); value && *value) state.folder = std::filesystem::path(value);
#endif
        if (state.folder) std::cout << "shaders: read from " << state.folder->string() << " where found there" << std::endl;
    }
    return state.folder;
}

} // namespace

void set_shader_directory(const std::filesystem::path& folder) {
    auto& state = shader_folder();
    const std::lock_guard guard{state.lock};
    state.chosen = true;
    state.read.clear();
    state.folder = folder.empty() ? std::nullopt : std::optional<std::filesystem::path>(folder);
    if (state.folder) std::cout << "shaders: read from " << state.folder->string() << " where found there" << std::endl;
}

std::optional<std::filesystem::path> shader_directory() {
    auto& state = shader_folder();
    const std::lock_guard guard{state.lock};
    return folder_in_use(state);
}

namespace detail {

const char* shader_code(const std::string_view name) {
    const EmbeddedShader* built_in = nullptr;
    for (const auto& shader : embedded_shaders) {
        if (shader.name == name) {
            built_in = &shader;
            break;
        }
    }
    if (!built_in) throw std::out_of_range("no shader named " + std::string(name));
    auto& state = shader_folder();
    const std::lock_guard guard{state.lock};
    const auto& folder = folder_in_use(state);
    if (!folder) return built_in->text;
    if (const auto found = state.read.find(name); found != state.read.end()) return found->second.c_str();
    const auto path = *folder / (std::string(name) + ".wgsl");
    std::ifstream file(path, std::ios::binary);
    if (!file) return built_in->text;
    std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    // As the built-in copy: without the carriage returns a checkout may add.
    std::erase(text, '\r');
    return state.read.emplace(std::string(name), std::move(text)).first->second.c_str();
}

} // namespace detail
} // namespace openrts::renderer
