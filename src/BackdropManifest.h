#pragma once

#include "BackdropPolicy.h"

#include <string>
#include <string_view>
#include <vector>

namespace MTB {
    // One optional set piece parsed from a [PieceN] section.
    struct ParsedPiece {
        std::string mesh;
        float       x = 0.0f, y = 0.0f, z = 0.0f;
        float       fitRadius = 0.0f;  // >0: scale bound to this; else use scale
        float       scale = 1.0f;
        float       yawDeg = 0.0f;
        bool        tint = false;
    };

    // A parsed backdrop manifest. Plain types only (no engine headers) so it is
    // unit-testable from string fixtures. valid=false means "skip this pack";
    // warnings explains why / what was dropped.
    struct ParsedPack {
        std::string id;      // filename stem (caller supplies)
        std::string name;    // [Pack] name (required)
        std::string author;  // [Pack] author (optional)

        bool        hasBackground = false;
        std::string bgImage;   // non-empty => image pack (DDS path)
        std::string bgDome;    // else a dome mesh path
        float       bgRadius = BackdropPolicy::kBackgroundRadiusDefault;
        float       bgZ = 0.0f;
        bool        bgFaceCamera = false;
        float       bgYaw = 0.0f;

        bool        hasStage = false;
        std::string floorMesh;
        float       floorRadius = 600.0f;
        float       floorZ = -10.0f;
        std::vector<ParsedPiece> extras;

        bool                     valid = false;
        std::vector<std::string> warnings;
    };

    // Parse a manifest's TEXT (the whole .ini as a string). Pure, no file IO.
    ParsedPack ParseBackdropManifest(std::string_view a_iniText, std::string_view a_packId);
}
