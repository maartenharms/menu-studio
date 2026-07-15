#include "PCH.h"

#include "BackdropPacks.h"

#include "BackdropManifest.h"

#include <array>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
    using namespace MTB;

    // Built-in backgrounds/stages (moved verbatim from Settings.cpp; their const
    // char* are string literals, so they are stable without interning).
    constexpr std::array<BackgroundPreset, 5> kBuiltinBackgrounds{ {
        { "blank", "mtb\\voidcolor.nif", 2000.0f, 0.0f, "", false, 0.0f },
        { "constellation", "interface\\intperkskydome.nif", 2000.0f, 0.0f, "", false, 0.0f },
        { "vampire", "dlc01\\interface\\intvampireperkskydome.nif", 2000.0f, 0.0f, "", false, 0.0f },
        { "teat", "interface\\teatperkskydome.nif", 2000.0f, 0.0f, "", false, 0.0f },
        { "custom", "mtb\\voidimage.nif", 2000.0f, 0.0f, "", false, 0.0f },
    } };
    constexpr std::array<StagePreset, 1> kBuiltinStages{ {
        { "starlight", "clutter\\nightingale\\nightingaleplatform.nif", 600.0f, -10.0f, {} },
    } };

    // The shared textured sphere reused for image packs (its clone's texture is
    // repointed at Apply time - see Backdrop.cpp).
    constexpr const char* kImageSphereMesh = "mtb\\voidimage.nif";

    // Owned storage. std::deque keeps element addresses stable across pushes, so
    // interned const char* and the extras spans stay valid until the next Scan().
    std::deque<std::string>             g_strings;
    std::deque<std::vector<StagePiece>> g_extraStore;
    std::vector<BackgroundPreset>       g_backgrounds;
    std::vector<StagePreset>            g_stages;
    std::unordered_map<std::string, std::string> g_authorByName;

    const char* Intern(const std::string& a_s) {
        g_strings.push_back(a_s);
        return g_strings.back().c_str();
    }

    std::filesystem::path BackdropsDir() {
        // Relative to the game root (the process CWD), same base the mod reads
        // MenuStudio.ini from. MO2's VFS merges every mod's files here.
        return std::filesystem::path{ "Data" } / "SKSE" / "Plugins" / "MenuStudio" / "Backdrops";
    }

    // Convert a path to a UTF-8 std::string WITHOUT the active-code-page
    // narrowing that path::string() does (that narrowing throws std::system_error
    // on a filename the code page cannot represent, e.g. a non-Latin pack name).
    std::string PathToUtf8(const std::filesystem::path& a_path) {
        const auto s = a_path.u8string();
        return std::string(reinterpret_cast<const char*>(s.data()), s.size());
    }
}

namespace MTB::BackdropPacks {
    void Scan() {
        g_strings.clear();
        g_extraStore.clear();
        g_backgrounds.clear();
        g_stages.clear();
        g_authorByName.clear();

        // Built-ins first (stable literals).
        for (const auto& b : kBuiltinBackgrounds) {
            g_backgrounds.push_back(b);
        }
        for (const auto& s : kBuiltinStages) {
            g_stages.push_back(s);
        }

        std::error_code ec;
        const auto dir = BackdropsDir();
        if (!std::filesystem::is_directory(dir, ec)) {
            spdlog::info("BackdropPacks: no '{}' folder; built-ins only.", dir.string());
            return;
        }

        int count = 0;
        try {
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                try {
                    if (!entry.is_regular_file(ec)) {
                        continue;
                    }
                    const auto& path = entry.path();
                    if (_wcsicmp(path.extension().c_str(), L".ini") != 0) {
                        continue;
                    }
                    const std::string fileLabel = PathToUtf8(path.filename());

                    std::ifstream in(path, std::ios::binary);
                    const std::string text{ std::istreambuf_iterator<char>(in),
                                            std::istreambuf_iterator<char>() };
                    const std::string id = PathToUtf8(path.stem());

                    ParsedPack pack = ParseBackdropManifest(text, id);
                    for (const auto& w : pack.warnings) {
                        spdlog::warn("BackdropPacks: {}: {}", fileLabel, w);
                    }
                    if (!pack.valid) {
                        continue;
                    }

                    const char* name = Intern(pack.name);
                    if (!pack.author.empty()) {
                        g_authorByName[pack.name] = pack.author;
                    }

                    if (pack.hasBackground) {
                        BackgroundPreset bg{};
                        bg.name = name;
                        if (!pack.bgImage.empty()) {
                            bg.mesh = kImageSphereMesh;
                            bg.image = Intern(pack.bgImage);
                            spdlog::warn("BackdropPacks: '{}' uses image=; a per-pack "
                                         "custom image is not supported in this build, so it "
                                         "shows the default sphere. Ship a dome=<nif> pointed "
                                         "at your texture for a custom image (see the readme).",
                                         fileLabel);
                        } else {
                            bg.mesh = Intern(pack.bgDome);
                            bg.image = "";
                        }
                        bg.radius = pack.bgRadius;
                        bg.z = pack.bgZ;
                        bg.faceCamera = pack.bgFaceCamera;
                        bg.yaw = pack.bgYaw;
                        g_backgrounds.push_back(bg);
                    }

                    if (pack.hasStage) {
                        g_extraStore.emplace_back();
                        auto& extras = g_extraStore.back();
                        for (const auto& e : pack.extras) {
                            extras.push_back(StagePiece{ Intern(e.mesh), e.fitRadius, e.scale,
                                                         e.x, e.y, e.z, e.yawDeg, e.tint });
                        }
                        StagePreset st{};
                        st.name = name;
                        st.floorMesh = Intern(pack.floorMesh);
                        st.floorRadius = pack.floorRadius;
                        st.floorZ = pack.floorZ;
                        st.extras = std::span<const StagePiece>{ extras };
                        g_stages.push_back(st);
                    }
                    ++count;
                } catch (const std::exception& e) {
                    spdlog::warn("BackdropPacks: skipped a pack file: {}", e.what());
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("BackdropPacks: directory scan aborted: {}", e.what());
        }
        spdlog::info("BackdropPacks: loaded {} pack(s) from '{}'.", count, dir.string());
    }

    std::span<const BackgroundPreset> Backgrounds() {
        return g_backgrounds;
    }

    std::span<const StagePreset> Stages() {
        return g_stages;
    }

    std::string_view AuthorOf(std::string_view a_presetName) {
        const auto it = g_authorByName.find(std::string{ a_presetName });
        return it == g_authorByName.end() ? std::string_view{} : std::string_view{ it->second };
    }
}
