#!/usr/bin/env python3
"""Generate the void-backdrop meshes + textures under dist/.

Two opaque inverted spheres, both self-lit (black diffuse so the studio rig
never washes them; the EMISSIVE channel carries the look) and both rendering
in the OPAQUE pass (Z-write, no alpha property) so they always occlude the
sky, day or night - the r57 fix that killed the two-surface sort conflict.

  voidshell.nif  - glow map OFF, flat emissive. The always-present shell: the
                   runtime tints its emissive to the void colour, so it is a
                   flat colour behind the constellation domes ("blank" = just
                   this, pure flat).
  voidimage.nif  - glow map ON, emissive WHITE, samples voidshell_g.dds. The
                   "custom" background: a picture wrapped on the sphere, shown
                   as authored (Backdrop leaves it untinted). Drop your own
                   equirectangular image over voidshell_g.dds to use it; it is
                   framed consistently by "Lock background angle".

Textures (uncompressed BGRA8, no external tools):
  voidshell_d.dds - black diffuse (kills lit wash; emissive is the look)
  voidshell_n.dds - flat tangent-space normal
  voidshell_g.dds - the EXAMPLE nebula+starfield (glow map for voidimage)

Skyrim LE stream (20.2.0.7 / user 12 / BSVer 83) via pyffi - SSE loads
LE-format NiTriShape geometry natively (HANDOFF lesson 26). Regenerate:
    python tools/make_voidshell.py
"""

import math
import os
import struct
import sys

import numpy as np
from pyffi.formats.nif import NifFormat

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MESH_DIR = os.path.join(ROOT, "dist", "meshes", "mtb")
TEX_DIR = os.path.join(ROOT, "dist", "textures", "mtb")
DIFFUSE_PATH = os.path.join(TEX_DIR, "voidshell_d.dds")
NORMAL_PATH = os.path.join(TEX_DIR, "voidshell_n.dds")
GLOW_PATH = os.path.join(TEX_DIR, "voidshell_g.dds")   # produced from the PNG by Texconv
GLOW_PNG = os.path.join(TEX_DIR, "voidshell_g.png")    # nebula source; Texconv -> BC7+mips DDS
WHITE_PATH = os.path.join(TEX_DIR, "voidshell_w.dds")  # flat white; effect baseColor tints it

RADIUS = 100.0
SEGMENTS = 32
RINGS = 24


def build_sphere():
    """Inverted UV sphere: inward normals, SEAM-SPLIT equirectangular UVs (the
    longitude seam duplicated so a wrapped texture shows no seam line), inward
    winding (we stand inside)."""
    cols = SEGMENTS + 1
    verts, uvs, normals = [], [], []
    verts.append((0.0, 0.0, RADIUS)); uvs.append((0.5, 0.0)); normals.append((0.0, 0.0, -1.0))
    for i in range(1, RINGS):
        lat = math.pi * i / RINGS
        z, r = RADIUS * math.cos(lat), RADIUS * math.sin(lat)
        for j in range(cols):
            lon = 2.0 * math.pi * j / SEGMENTS
            x, y = r * math.cos(lon), r * math.sin(lon)
            verts.append((x, y, z))
            uvs.append((j / SEGMENTS, i / RINGS))
            n = 1.0 / RADIUS
            normals.append((-x * n, -y * n, -z * n))
    verts.append((0.0, 0.0, -RADIUS)); uvs.append((0.5, 1.0)); normals.append((0.0, 0.0, 1.0))
    top, bottom = 0, len(verts) - 1

    def ring(i, j):
        return 1 + (i - 1) * cols + j

    tris = []

    def add(a, b, c):
        tris.append((a, c, b))                # inward-facing winding

    for j in range(SEGMENTS):
        add(top, ring(1, j), ring(1, j + 1))
    for i in range(1, RINGS - 1):
        for j in range(SEGMENTS):
            a, b = ring(i, j), ring(i, j + 1)
            c, d = ring(i + 1, j), ring(i + 1, j + 1)
            add(a, c, b); add(b, c, d)
    for j in range(SEGMENTS):
        add(bottom, ring(RINGS - 1, j + 1), ring(RINGS - 1, j))
    return verts, uvs, normals, tris


def make_nif(path, root_name, mode):
    """mode 'shell' = OPAQUE lighting sphere (flat emissive, runtime-tinted to
    the void colour, Z-write → occludes the sky). mode 'image' = UNLIT EFFECT
    sphere sampling the nebula directly. The BSLightingShader GLOW-MAP technique
    CTDs in the d3d11 draw on our converted stream-83 mesh (r58: two identical
    AVs, BC7+mips did NOT help - it's the glow technique, not the texture), so
    the custom image uses the proven effect-shader path (the original shell's).
    It sits INSIDE the opaque lighting shell, which owns sky occlusion, so the
    effect shader's transparent-pass rendering is harmless here."""
    verts, uvs, normals, tris = build_sphere()
    lighting = (mode == "shell")

    data = NifFormat.Data()
    data.version = 0x14020007
    data.user_version = 12
    data.user_version_2 = 83
    data.header.endian_type = 1               # pyffi default is big-endian

    root = NifFormat.BSFadeNode()
    root.name = root_name.encode()
    root.flags = 14
    root.rotation.set_identity()
    root.scale = 1.0

    shape = NifFormat.NiTriShape()
    shape.name = (root_name + ":0").encode()
    shape.flags = 14
    shape.rotation.set_identity()
    shape.scale = 1.0

    geom = NifFormat.NiTriShapeData()
    geom.has_vertices = True
    geom.num_vertices = len(verts)
    geom.vertices.update_size()
    for slot, (x, y, z) in zip(geom.vertices, verts):
        slot.x, slot.y, slot.z = x, y, z
    geom.has_normals = lighting           # the effect image is unlit - no normals
    if lighting:
        geom.normals.update_size()
        for slot, (x, y, z) in zip(geom.normals, normals):
            slot.x, slot.y, slot.z = x, y, z
    geom.has_vertex_colors = False
    geom.num_uv_sets = 1
    geom.has_uv = True
    geom.uv_sets.update_size()
    for slot, (u, v) in zip(geom.uv_sets[0], uvs):
        slot.u, slot.v = u, v
    geom.has_triangles = True
    geom.num_triangles = len(tris)
    geom.num_triangle_points = len(tris) * 3
    geom.triangles.update_size()
    for slot, (a, b, c) in zip(geom.triangles, tris):
        slot.v_1, slot.v_2, slot.v_3 = a, b, c
    geom.consistency_flags = NifFormat.ConsistencyType.CTSTATIC

    shape.data = geom
    if lighting:
        shape.update_tangent_space(as_extra=False)
    geom.update_center_radius()

    shape.bs_properties.update_size()
    if lighting:
        # OPAQUE lighting sphere: flat emissive (runtime-tinted to the void
        # colour), Z-write, no alpha property → renders in the opaque pass and
        # occludes the sky. Diffuse black + own-emit so the rig can't wash it.
        texset = NifFormat.BSShaderTextureSet()
        texset.num_textures = 9
        texset.textures.update_size()
        texset.textures[0] = b"textures\\mtb\\voidshell_d.dds"   # diffuse (black)
        texset.textures[1] = b"textures\\mtb\\voidshell_n.dds"   # normal (flat)
        lsp = NifFormat.BSLightingShaderProperty()
        lsp.skyrim_shader_type = NifFormat.BSLightingShaderPropertyShaderType.Default
        lsp.texture_set = texset
        lsp.shader_flags_1.slsf_1_own_emit = 1
        lsp.shader_flags_1.slsf_1_z_buffer_test = 1
        lsp.shader_flags_1.slsf_1_recieve_shadows = 0
        lsp.shader_flags_1.slsf_1_cast_shadows = 0
        lsp.shader_flags_1.slsf_1_specular = 0
        lsp.shader_flags_2.slsf_2_z_buffer_write = 1
        lsp.shader_flags_2.slsf_2_double_sided = 1
        lsp.shader_flags_2.slsf_2_vertex_colors = 0
        lsp.uv_scale.u = lsp.uv_scale.v = 1.0
        lsp.texture_clamp_mode = 3
        lsp.emissive_color.r = lsp.emissive_color.g = lsp.emissive_color.b = 1.0
        lsp.emissive_multiple = 1.0
        lsp.alpha = 1.0
        lsp.glossiness = 20.0
        lsp.specular_color.r = lsp.specular_color.g = lsp.specular_color.b = 1.0
        lsp.specular_strength = 0.0
        lsp.environment_map_scale = 1.0
        shape.bs_properties[0] = lsp
    else:
        # UNLIT EFFECT sphere (the proven render path, no glow-map technique
        # which CTDs). 'image' samples the nebula shown as-authored; 'color'
        # samples flat white so the runtime exact-tint paints it the void
        # colour (r59: the lighting shell's flat emissive never rendered - the
        # effect baseColor tint is how the void colour actually shows). r44:
        # effect geometry needs an alpha property to be accumulated; texture
        # alpha is 255, so with src/inv-src blend it stays opaque and covers
        # the shell (which owns ENB-sky occlusion behind it).
        fx = NifFormat.BSEffectShaderProperty()
        fx.shader_flags_1.slsf_1_z_buffer_test = 1
        fx.shader_flags_2.slsf_2_z_buffer_write = 1
        fx.shader_flags_2.slsf_2_double_sided = 1
        fx.shader_flags_2.slsf_2_vertex_colors = 0
        fx.uv_scale.u = fx.uv_scale.v = 1.0
        fx.source_texture = (b"textures\\mtb\\voidshell_g.dds" if mode == "image"
                             else b"textures\\mtb\\voidshell_w.dds")
        fx.texture_clamp_mode = 3
        fx.emissive_color.r = fx.emissive_color.g = fx.emissive_color.b = 1.0
        fx.emissive_color.a = 1.0
        fx.emissive_multiple = 1.0
        alpha = NifFormat.NiAlphaProperty()
        alpha.flags = 4845
        alpha.threshold = 128
        shape.bs_properties[0] = fx
        shape.bs_properties[1] = alpha

    root.num_children = 1
    root.children.update_size()
    root.children[0] = shape

    data.roots = [root]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as stream:
        data.write(stream)
    return len(verts), len(tris)


def _write_dds(path, bgra):
    """Uncompressed BGRA8 DDS from an (H,W,4) uint8 array (B,G,R,A order)."""
    h, w = bgra.shape[:2]
    DDSD = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000      # CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT
    header = struct.pack(
        "<4s7I44x8I5I",
        b"DDS ", 124, DDSD, h, w, w * 4, 0, 0,  # magic,size,flags,h,w,pitch,depth,mips
        32, 0x41, 0, 32,                        # pf: size, RGB|ALPHAPIXELS, fourcc, bpp
        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000,
        0x1000, 0, 0, 0, 0)                     # caps: TEXTURE
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as stream:
        stream.write(header)
        stream.write(np.ascontiguousarray(bgra).tobytes())


def _solid(rgb, size=4):
    r, g, b = rgb
    img = np.zeros((size, size, 4), np.uint8)
    img[..., 0] = b; img[..., 1] = g; img[..., 2] = r; img[..., 3] = 255
    return img


def _fbm(h, w, exponent, seed):
    """Periodic cloud noise via a 1/f^exponent-filtered FFT of white noise -
    wraps on both axes (so the equirectangular longitude seam matches), no
    external noise lib. Returns floats in [0,1]."""
    rng = np.random.default_rng(seed)
    spec = np.fft.fft2(rng.standard_normal((h, w)))
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    radius = np.sqrt(fy * fy + fx * fx)
    radius[0, 0] = 1.0
    spec *= 1.0 / (radius ** exponent)
    field = np.real(np.fft.ifft2(spec))
    field -= field.min()
    field /= max(field.max(), 1e-6)
    return field


def make_nebula(w=2048, h=1024):
    """Example equirectangular nebula + starfield (BGRA8). Colour clouds from a
    blue/violet/magenta ramp over FFT cloud noise, a brighter core mask, and
    scattered stars. Replace voidshell_g.dds with any 2:1 image to use your
    own. Poles (top/bottom rows) fade to near-black to hide the UV pinch."""
    clouds = _fbm(h, w, 2.2, seed=7)
    detail = _fbm(h, w, 3.0, seed=19)
    density = np.clip(clouds * 0.85 + detail * 0.35 - 0.15, 0.0, 1.0)
    core = np.clip((density - 0.45) / 0.55, 0.0, 1.0) ** 1.5   # bright dense cores

    # colour ramp: deep space -> violet -> magenta/pink cores
    deep = np.array([6, 4, 14])       # RGB
    mid = np.array([40, 22, 82])
    hot = np.array([150, 70, 130])
    d = density[..., None]
    rgb = deep + (mid - deep) * d + (hot - mid) * core[..., None]
    rgb += (np.array([210, 180, 230]) - rgb) * (core[..., None] ** 3) * 0.6

    # fade the poles to hide the single-vertex pinch
    lat = np.linspace(0.0, 1.0, h)[:, None]
    polefade = np.clip(np.sin(lat * math.pi) * 1.15, 0.0, 1.0)
    rgb *= polefade[..., None]

    # stars: sparse bright points, a few larger
    rng = np.random.default_rng(101)
    stars = (rng.random((h, w)) > 0.9975).astype(np.float32)
    bright = rng.random((h, w)) * 0.5 + 0.5
    star_add = (stars * bright * 255.0)[..., None] * np.array([1.0, 1.0, 1.05])
    rgb = np.clip(rgb + star_add * polefade[..., None], 0, 255)
    return rgb.astype(np.uint8)                         # RGB (H,W,3)


def make_textures():
    # Diffuse + normal stay tiny uncompressed DDS (4x4, no mips needed at that
    # size - voidshell.nif proves they sample fine). The GLOW is 2048x1024:
    # a large texture MUST be BC-compressed with a full mip chain or Skyrim's
    # trilinear sampler reads a non-existent mip and the lighting-shader draw
    # AVs in d3d11 (r58 CTD). So write it as a PNG here; Texconv makes the DDS.
    _write_dds(DIFFUSE_PATH, _solid((0, 0, 0)))         # black diffuse
    _write_dds(NORMAL_PATH, _solid((128, 128, 255)))    # flat normal
    _write_dds(WHITE_PATH, _solid((255, 255, 255)))     # white: effect tint base
    from PIL import Image
    Image.fromarray(make_nebula(), "RGB").save(GLOW_PNG)


def verify(path):
    check = NifFormat.Data()
    with open(path, "rb") as stream:
        check.read(stream)
    shape = check.roots[0].children[0]
    prop = shape.bs_properties[0]
    alpha = shape.bs_properties[1]
    print("  verify %s: verts=%d tris=%d normals=%d prop=%s alphaProp=%s"
          % (os.path.basename(path), shape.data.num_vertices, shape.data.num_triangles,
             len(shape.data.normals) if shape.data.normals else 0,
             type(prop).__name__, type(alpha).__name__ if alpha else "NONE"))


# DirectXTex (ships with SSEEdit in the Nolvus tools) - turns the nebula PNG
# into a Skyrim-correct BC7 DDS with a full mip chain.
TEXCONV = (r"C:\Games\Nolvus\Instances\Nolvus Awakening\TOOLS\SSE Edit"
           r"\Edit Scripts\Texconv.exe")


def compress_glow():
    import shutil
    import subprocess
    exe = TEXCONV if os.path.exists(TEXCONV) else shutil.which("texconv")
    if not exe:
        print("  WARNING: Texconv not found - voidshell_g.dds NOT rebuilt. Run:")
        print("    texconv -f BC7_UNORM -m 0 -y -o <texdir> voidshell_g.png")
        return False
    subprocess.run([exe, "-f", "BC7_UNORM", "-m", "0", "-y", "-o", TEX_DIR,
                    GLOW_PNG], check=True, capture_output=True)
    # Texconv writes an UPPERCASE extension (voidshell_g.DDS). The meshes
    # reference lowercase (voidshell_g.dds) - Windows resolves either, but a
    # case-sensitive FS (Linux/Proton) would not find it. Force the canonical
    # lowercase name (via a temp step; a direct case-only rename is unreliable
    # on Windows).
    if os.path.exists(GLOW_PATH):
        tmp = GLOW_PATH + ".tmp"
        os.replace(GLOW_PATH, tmp)
        os.replace(tmp, GLOW_PATH)
    return os.path.exists(GLOW_PATH)


if __name__ == "__main__":
    for name, mode, fn in (("MTB_VoidShell", "shell", "voidshell.nif"),
                           ("MTB_VoidImage", "image", "voidimage.nif"),
                           ("MTB_VoidColor", "color", "voidcolor.nif")):
        p = os.path.join(MESH_DIR, fn)
        nv, nt = make_nif(p, name, mode)
        print("%-14s %d verts, %d tris -> %s (%d bytes)"
              % (os.path.basename(p), nv, nt, p, os.path.getsize(p)))
        verify(p)
    make_textures()
    ok = compress_glow()
    for p in (DIFFUSE_PATH, NORMAL_PATH, GLOW_PNG):
        print("  %s (%d bytes)" % (os.path.relpath(p, ROOT), os.path.getsize(p)))
    if ok:
        print("  %s (%d bytes) [BC7 + mips]"
              % (os.path.relpath(GLOW_PATH, ROOT), os.path.getsize(GLOW_PATH)))
    sys.exit(0 if ok else 1)
