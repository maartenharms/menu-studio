#include "BackdropManifest.h"

#include <SimpleIni.h>

#include <string>

namespace {
    float GetF(const CSimpleIniA& a_ini, const char* a_sec, const char* a_key, float a_def) {
        const char* raw = a_ini.GetValue(a_sec, a_key, nullptr);
        if (!raw || !*raw) {
            return a_def;
        }
        try {
            return std::stof(raw);
        } catch (...) {
            return a_def;
        }
    }

    bool GetB(const CSimpleIniA& a_ini, const char* a_sec, const char* a_key, bool a_def) {
        const char* raw = a_ini.GetValue(a_sec, a_key, nullptr);
        if (!raw || !*raw) {
            return a_def;
        }
        switch (raw[0]) {
        case '1': case 't': case 'T': case 'y': case 'Y':
            return true;
        default:
            return false;
        }
    }

    std::string GetS(const CSimpleIniA& a_ini, const char* a_sec, const char* a_key) {
        const char* raw = a_ini.GetValue(a_sec, a_key, nullptr);
        return raw ? std::string{ raw } : std::string{};
    }

    bool HasSection(const CSimpleIniA& a_ini, const char* a_sec) {
        return a_ini.GetSectionSize(a_sec) >= 0;  // -1 when the section is absent
    }
}

namespace MTB {
    ParsedPack ParseBackdropManifest(std::string_view a_iniText, std::string_view a_packId) {
        ParsedPack pack;
        pack.id = std::string{ a_packId };

        CSimpleIniA ini;
        ini.SetUnicode();
        ini.SetMultiKey(false);
        if (ini.LoadData(a_iniText.data(), a_iniText.size()) < 0) {
            pack.warnings.emplace_back("manifest is not valid INI");
            return pack;
        }

        pack.name = GetS(ini, "Pack", "name");
        pack.author = GetS(ini, "Pack", "author");
        if (pack.name.empty()) {
            pack.warnings.emplace_back("missing [Pack] name");
            return pack;
        }

        if (HasSection(ini, "Background")) {
            const std::string image = GetS(ini, "Background", "image");
            const std::string dome = GetS(ini, "Background", "dome");
            if (image.empty() && dome.empty()) {
                pack.warnings.emplace_back("[Background] has neither image nor dome; ignored");
            } else {
                pack.hasBackground = true;
                if (!image.empty() && !dome.empty()) {
                    pack.warnings.emplace_back("[Background] has both image and dome; using image");
                }
                if (!image.empty()) {
                    pack.bgImage = image;
                    pack.bgFaceCamera = GetB(ini, "Background", "faceCamera", true);
                } else {
                    pack.bgDome = dome;
                    pack.bgFaceCamera = GetB(ini, "Background", "faceCamera", false);
                }
                pack.bgRadius = GetF(ini, "Background", "radius", 2200.0f);
                pack.bgZ = GetF(ini, "Background", "z", 0.0f);
                pack.bgYaw = GetF(ini, "Background", "yaw", 0.0f);
            }
        }

        if (HasSection(ini, "Stage")) {
            const std::string floor = GetS(ini, "Stage", "floor");
            if (floor.empty()) {
                pack.warnings.emplace_back("[Stage] has no floor; ignored");
            } else {
                pack.hasStage = true;
                pack.floorMesh = floor;
                pack.floorRadius = GetF(ini, "Stage", "radius", 600.0f);
                pack.floorZ = GetF(ini, "Stage", "z", -10.0f);
                for (int i = 1;; ++i) {
                    const std::string sec = "Piece" + std::to_string(i);
                    if (!HasSection(ini, sec.c_str())) {
                        break;
                    }
                    const std::string mesh = GetS(ini, sec.c_str(), "mesh");
                    if (mesh.empty()) {
                        pack.warnings.push_back(sec + " has no mesh; skipped");
                        continue;
                    }
                    ParsedPiece pc;
                    pc.mesh = mesh;
                    pc.x = GetF(ini, sec.c_str(), "x", 0.0f);
                    pc.y = GetF(ini, sec.c_str(), "y", 0.0f);
                    pc.z = GetF(ini, sec.c_str(), "z", 0.0f);
                    pc.fitRadius = GetF(ini, sec.c_str(), "fit", 0.0f);
                    pc.scale = GetF(ini, sec.c_str(), "scale", 1.0f);
                    pc.yawDeg = GetF(ini, sec.c_str(), "yaw", 0.0f);
                    pc.tint = GetB(ini, sec.c_str(), "tint", false);
                    pack.extras.push_back(std::move(pc));
                }
            }
        }

        if (!pack.hasBackground && !pack.hasStage) {
            pack.warnings.emplace_back("pack defines neither a usable background nor stage");
            return pack;
        }
        pack.valid = true;
        return pack;
    }
}
