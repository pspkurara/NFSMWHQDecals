// NFSMW HQ Decals - a body-like gloss for the decals of NFS Most Wanted (speed.exe 1.3). Copyright (C) 2026 pspkurara, GPL-3.0 (see LICENSE).
//
// What this file does
// -------------------
// The game draws number decals and body/side/window decals with the same pixel shader as the car body, but with constants that
// make them look flat. This plug-in redraws only those decals with a copy of that shader that has a few extra controls
// (reflection, highlight, metallic), set from NFSMWHQDecals.ini.
//
// The game's own car shader is a ps_1_1 shader (kShader11). It has no normal to work with, so it gets a reduced set of controls.
// Mods such as REDUX replace the car shader with a ps_3_0 one (kShader). For that the plug-in offers more: the gloss can follow the
// body's normal and the body's highlight. This is the "mod support" part; it is switched on by the hash of the shader.
//
// Steps
//  1. Find the decal draws. Two game functions are hooked: CarRenderInfo::RenderPart (0x7387E0) says which car part is being
//     drawn, and the mesh submit function (0x6E0AB0) queues a render item for it. The item holds the index buffer, the start index
//     and the triangle count. Those three values are what Direct3D later receives in DrawIndexedPrimitive, so a decal draw is
//     recognised by them, not by its shader. Part names tell the kind: "DECAL" in the name is a decal, "WINDOW" a window decal,
//     and a texture called NUMBER_* is a number decal.
//  2. Draw them again with our shader. In DrawIndexedPrimitive (hooked), a recognised decal draw is issued with the replacement
//     pixel shader instead of the game's. With Strength=0 the result is the same as the game's own.
//  3. Mod shader only: make the gloss follow the body. The decal mesh has smooth normals of its own, but the body's normals break
//     at press lines. Each opaque car draw is therefore drawn a second time into an extra render target as its normal (the "normal
//     pre-pass"). A decal pixel reads the body's normal at its own screen position from that target and uses it for the gloss.
//  4. Settings come from NFSMWHQDecals.ini, which can be re-read every second (HotReload).
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "MinHook.h"

namespace {
// Addresses in speed.exe 1.3 (the 32-bit PC executable). They do not exist in other builds.
constexpr uintptr_t kDevicePtr = 0x982BDC;       // the game's IDirect3DDevice9*
constexpr uintptr_t kQueueCount = 0x982CDC;       // render queue: number of entries
constexpr uintptr_t kQueueEntries = 0x9877B4;     // render queue: entries of {item index, sort key}, 8 bytes each
constexpr uintptr_t kItems = 0x93E878;            // render items, 0x44 bytes each
constexpr uint32_t kCarPixelShaderHash = 0x5F530544; // FNV-1a hash of the bytecode of the game's car pixel shader (ps_3_0, REDUX)
constexpr uint32_t kCarPixelShaderHash11 = 0xA33FF169; // the same for the old ps_1_1 car shader (stock game, low shader detail)

// What kind of draw a decal is. The number is also the index into g_stat.
enum Kind : uint8_t { kNone = 0, kNumber = 1, kDecal = 2, kWindow = 3 };

// The look of one decal group ([NumberDecal] or [BodyWindowDecal] in the INI). See the INI for what each value does.
struct Group {
    bool enabled = true;
    float strength = 1.0f, smoothness = 1.0f, specular = 0.3f, reflectivity = 0.25f, metallic = 0.0f, fresnel = 0.0f;
    bool matchBody = false;
    float ignoreMask = 1.0f; // 0..1: how much the decal mesh's own vertex-colour masks are ignored (1 = fully)
    float highlightMax = 0.5f; // ceiling of the highlight added to the decal: 0 = none, infinity = no ceiling
    bool energy = false;     // true: reflection replaces part of the colour (never clips); false: reflection is added on top
};
// The [Main] settings and the two groups.
struct Config {
    bool enabled = true;
    bool log = false;
    bool hotReload = false;      // re-read the INI about every second
    bool debug = false;          // tint the replaced draws magenta
    bool logParts = false;       // log every part name and number textures on parts not treated as decals
    bool debugNormals = false;   // show the normals as colours
    bool surfaceNormals = true;  // use the body's normals under each decal pixel (needs the normal pre-pass)
    float highlightMax = 0.5f;   // ceiling of the highlight added to a decal: 0 = none, infinity = no ceiling
    bool bodySpecular = true;    // use the body's highlight strength under each decal pixel (needs the normal pre-pass)
    bool compensate = true;      // on the ps_3_0 (REDUX) shader: make the highlight look the same as with the game's own shader
    Group number, bodyWindow;
} g_cfg;

std::filesystem::path g_dir, g_ini, g_logPath;   // plug-in folder, INI file, log file
FILETIME g_iniTime{};                              // last write time of the INI when it was read
ULONGLONG g_nextIniCheck = 0;                      // next time (GetTickCount64) HotReload looks at the INI

void log(const std::string& s) {
    if (!g_cfg.log) return;
    std::ofstream(g_logPath, std::ios::app) << s << '\n';
}

// ---- INI -------------------------------------------------------------------------------------------------------------------
// Readers for one value. A missing or empty key gives the default.
// A value as text, without a trailing "// comment" and without spaces at the ends. Empty if the key is missing.
std::string getS(const char* sec, const char* key) {
    char buf[256];
    GetPrivateProfileStringA(sec, key, "", buf, sizeof buf, g_ini.string().c_str());
    std::string v = buf;
    const size_t c = v.find("//");
    if (c != std::string::npos) v.erase(c);
    const size_t first = v.find_first_not_of(" 	");
    const size_t last = v.find_last_not_of(" 	");
    return first == std::string::npos ? std::string() : v.substr(first, last - first + 1);
}
float getF(const char* sec, const char* key, float def) {
    const std::string v = getS(sec, key);
    return v.empty() ? def : static_cast<float>(atof(v.c_str()));
}
bool getB(const char* sec, const char* key, bool def) {
    const std::string v = getS(sec, key);
    return v.empty() ? def : atoi(v.c_str()) != 0;
}
// HighlightMax text: empty = `def`, "Unlimited" (or Infinity, inf) = no ceiling, a number = the ceiling (0 = none).
float parseHighlight(std::string v, float def) {
    if (v.empty()) return def;
    for (auto& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (v == "unlimited" || v == "infinity" || v == "inf") return INFINITY;
    return std::max(0.0f, static_cast<float>(atof(v.c_str())));
}
void readGroup(const char* sec, Group& g, float defaultHighlightMax) {
    g.enabled = getB(sec, "Enabled", g.enabled);
    g.strength = std::clamp(getF(sec, "Strength", g.strength), 0.0f, 1.0f);
    g.smoothness = std::clamp(getF(sec, "Smoothness", g.smoothness), 0.0f, 1.0f);
    g.specular = std::max(0.0f, getF(sec, "Specular", g.specular));
    g.reflectivity = std::max(0.0f, getF(sec, "Reflectivity", g.reflectivity));
    g.metallic = std::clamp(getF(sec, "Metallic", g.metallic), 0.0f, 1.0f);
    g.fresnel = std::clamp(getF(sec, "Fresnel", g.fresnel), 0.0f, 1.0f);
    g.matchBody = getB(sec, "MatchBody", g.matchBody);
    g.ignoreMask = std::clamp(getF(sec, "IgnoreVertexMask", g.ignoreMask), 0.0f, 1.0f);
    g.energy = getB(sec, "EnergyConserving", g.energy);
    g.highlightMax = parseHighlight(getS(sec, "HighlightMax"), defaultHighlightMax);   // a group's own value wins over [Main]'s
}
// Written when there is no INI. Its text is NFSMWHQDecals.ini of the source tree, which CMake embeds as default_ini.inc.
void writeDefaultIni() {
    static const char kDefaultIni[] =
#include "default_ini.inc"
        ;
    std::ofstream(g_ini, std::ios::binary) << kDefaultIni;
}
// Reads the INI. Without `force` it does nothing unless the file changed since the last read.
void loadIni(bool force = false) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &a)) {
        writeDefaultIni();
        GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &a);
    } else if (!force && CompareFileTime(&a.ftLastWriteTime, &g_iniTime) == 0) {
        return;
    }
    g_iniTime = a.ftLastWriteTime;
    g_cfg.enabled = getB("Main", "Enabled", true);
    g_cfg.log = getB("Main", "Log", false);
    g_cfg.hotReload = getB("Main", "HotReload", false);
    g_cfg.debug = getB("Main", "Debug", false);
    g_cfg.debugNormals = getB("Main", "DebugNormals", false);
    g_cfg.logParts = getB("Main", "LogParts", false);
    g_cfg.surfaceNormals = getB("Main", "SurfaceNormals", true);
    g_cfg.bodySpecular = getB("Main", "BodySpecular", true);
    g_cfg.compensate = getB("Main", "Compensate", true);
    g_cfg.highlightMax = parseHighlight(getS("Main", "HighlightMax"), 0.5f);   // used by a group that has no HighlightMax of its own
    readGroup("NumberDecal", g_cfg.number, g_cfg.highlightMax);
    readGroup("BodyWindowDecal", g_cfg.bodyWindow, g_cfg.highlightMax);
}

// ---- Replacement pixel shader -------------------------------------------------------------------------------------------------
// The ps_3_0 car shader that mods such as REDUX put in place of the game's own (55 instructions), rewritten in HLSL, plus the
// extra controls. Hash 0x5F530544; it is a mod shader, so this part is the "mod support".
// The game's shader reads: s0 diffuse texture, s1 noise volume, s2 environment cube map, s3 shadow map; c0-c2 light colours
// (LocalColourMatrix), c3 SpecularHotSpot, c4 SpecularPower, c5/c6 ambient and shadow colour.
// Our controls are passed in c20-c23 (set per draw in dipImpl, see `uni` there):
//   c20 = strength, hotspot, power, reflectivity     c21 = metallic, fresnel, debug tint, ignore-mask
//   c22 = energy-conserving, debug normals, body-highlight on, highlight ceiling     c23 = 1/size of the normal target, flags
// c24-c26 hold the WorldView rows (needed to build the cube map direction from the body's normal). c27.x scales the whole highlight.
const char* kShader = R"(
sampler2D S0 : register(s0);
sampler3D S1 : register(s1);
samplerCUBE S2 : register(s2);
sampler2D S3 : register(s3);
sampler2D S4 : register(s4);   // the normal pre-pass target: the body's normals (alpha 0 where there is no body)
float4 uD : register(c23);     // x,y = 1 / render target size; z = S4 is valid this frame; w = show normals (debug)
float4 uE0 : register(c24);    // rows of WorldView (the vertex shader's c8-c10): turn a local direction into the cube map's lookup direction
float4 uE1 : register(c25);
float4 uE2 : register(c26);
float4 gC0 : register(c0);
float4 gC1 : register(c1);
float4 gC2 : register(c2);
float4 gC3 : register(c3);
float4 gC4 : register(c4);
float4 gC5 : register(c5);
float4 gC6 : register(c6);
float4 uA : register(c20);
float4 uB : register(c21);
float4 uC : register(c22);
float4 uF : register(c27);     // x = scale of the whole highlight (Specular, doubled by the compensation)
struct PSIn {
  float4 col0 : COLOR0; float4 col1 : COLOR1;
  float3 envDir : TEXCOORD0; float2 uv : TEXCOORD1; float4 shadow : TEXCOORD2; float4 light : TEXCOORD3;
  float3 nrm : TEXCOORD4; float3 view : TEXCOORD5; float3 vol : TEXCOORD6;
};
float4 main(PSIn i, float2 vpos : VPOS) : COLOR0 {
  // The body's normal under this pixel, taken from the pre-pass target. Without it, the decal mesh's own normal is used.
  float3 bodyN = normalize(i.nrm);
  float bodySpec = -1.0;   // the body's highlight strength under this pixel (-1 = not known)
  if (uD.z > 0.5) {
    float4 sn = tex2D(S4, (vpos + 0.5) * uD.xy);
    if (sn.a > 0.25) { bodyN = normalize(sn.rgb * 2.0 - 1.0); if (uC.z > 0.5) bodySpec = (sn.a - 0.5) * 4.0; }
  }
  float3 noise = (tex3Dbias(S1, float4(i.vol, -3.0)).xyz - 0.5) * i.light.w * 2.0;
  // NO: normal for the game's own result (decal mesh normal). NP: normal for our result (body normal under the pixel).
  float3 NO = normalize(normalize(i.nrm) + noise);
  float3 NP = normalize(bodyN + noise);
  float3 L = normalize(i.light.xyz);   // despite the name: the direction to the eye (the vertex shader's LocalEyePos - position)
  float lgO = log2(saturate(dot(NO * (dot(NO, i.view) * 2.0) - i.view, L)));
  float lgP = log2(saturate(dot(NP * (dot(NP, i.view) * 2.0) - i.view, L)));
  float NdE = saturate(dot(NP, L));
  // The vertex shader makes the cube map lookup direction from the decal mesh's normal. Make it again from the body's normal,
  // with the same steps: reflect the eye direction (R = 2 max(N.E, 0.01) N - E), then rotate with the WorldView rows.
  float3 envDirP = i.envDir;
  if (uD.z > 0.5) {
    float3 Rv = bodyN * (2.0 * max(dot(bodyN, L), 0.01)) - L;
    envDirP = float3(dot(Rv, uE0.xyz), dot(Rv, uE1.xyz), dot(Rv, uE2.xyz));
  }
  float s = tex2Dproj(S3, i.shadow + float4(0.00310272281, 0.00169502525, 0, 0)).x
          + tex2Dproj(S3, i.shadow + float4(-0.0059458795, 0.00146509614, 0, 0)).x
          + tex2Dproj(S3, i.shadow + float4(0.00438271556, -0.00657965057, 0, 0)).x
          + tex2Dproj(S3, i.shadow + float4(0.00143468124, 0.00924346782, 0, 0)).x;
  float sh = saturate(s * 0.25 + 0.1);
  float4 tex = tex2D(S0, i.uv);
  float3 albedo = i.col0.rgb * tex.rgb;
  float3 lc = float3(gC0.x, gC1.x, gC2.x);
  float3 env = texCUBE(S2, i.envDir).rgb * i.col0.w * i.col1.w;
  float3 amb = gC5.xyz + sh * gC6.xyz;

  // The game's own result: the original constants and a fixed reflection of 0.5. Used when Strength is 0.
  float spO = exp2(lgO * gC4.x) + 5.0 * sh * exp2(lgO * gC4.x * 80.0) * gC3.x;
  float3 litO = lc * spO * i.col0.w * i.col1.xyz * sh;
  float3 outO = (albedo + litO) * amb + env * 0.5;

  // Our result. spP: highlight shape (a broad lobe, plus a sharp spot scaled by uA.y, the hotspot).
  float spP = exp2(lgP * uA.z) + 5.0 * sh * exp2(lgP * uA.z * 80.0) * uA.y;
  float3 tint = lerp(float3(1, 1, 1), saturate(albedo), uB.x);
  // uB.w = how much the vertex colour masks are ignored. The decal mesh's own masks can be 0, which would remove highlight and reflection.
  float3 specMask = lerp(i.col0.w * i.col1.xyz, float3(1, 1, 1), uB.w);
  if (bodySpec >= 0.0) specMask = float3(bodySpec, bodySpec, bodySpec);   // the body's strength replaces the mask where it is known
  float3 litP = lc * tint * spP * specMask * sh * uF.x;
  if (uC.w < 0.0) litP = float3(0, 0, 0);   // HighlightMax=0: no added highlight (the body has none)
  else if (uC.w > 0.0) litP = uC.w * (1.0 - exp(-litP / uC.w));   // HighlightMax=n: soft ceiling at n, so a dark decal cannot turn white
  float F = lerp(1.0, 0.04 + 0.96 * pow(1.0 - NdE, 5.0), uB.y);
  float3 envRaw = texCUBE(S2, envDirP).rgb * lerp(i.col0.w * i.col1.w, 1.0, uB.w);
  float3 envP = envRaw * tint * (uA.w * F);
  float3 outP = (albedo * (1.0 - uB.x) + litP) * amb + envP;
  // Energy conserving: the reflected share replaces part of the decal colour instead of being added on top, so nothing clips.
  // The highlight stays additive.
  float share = saturate(uA.w * F);
  float3 outE = (albedo * (1.0 - uB.x) * (1.0 - share) + litP) * amb + envRaw * tint * share;
  outP = lerp(outP, outE, uC.x);

  // Debug view (DebugNormals): the normal as a colour. A decal that uses the body's normal blends into the body in this view.
  if (uC.y > 0.5) return float4((uD.w > 0.5 ? bodyN : normalize(i.nrm)) * 0.5 + 0.5, tex.a * i.col0.w);
  float3 rgb = lerp(outO, outP, uA.x);
  rgb = lerp(rgb, float3(1, 0, 1), uB.z * 0.5); // Debug: magenta tint on replaced draws
  return float4(rgb, tex.a * i.col0.w);
}
)";
IDirect3DPixelShader9* g_ps = nullptr;
bool g_psFailed = false;

// ---- Replacement for the old ps_1_1 car shader -------------------------------------------------------------------------------
// The stock game uses a ps_1_1 car shader (11 instructions). In HLSL (compiled as ps_2_0, which may be paired with the game's
// vs_1_1) it is:   colour = diffuse * COLOR0 + 2 * d^8 * COLOR0.w * COLOR1.rgb + environment * COLOR1.w * COLOR0.w
// where d = saturate(dot(t2, t3)) comes from two vectors the vertex shader sends in texture coordinates 2 and 3 (ps_1_1 clamps
// them to 0..1). This shader has no normal, view vector, ambient or shadow input, so only part of the controls can work:
// Strength, Specular (strength of the highlight, whose shape is fixed), Reflectivity, Metallic, IgnoreVertexMask,
// EnergyConserving and HighlightMax. Fresnel, MatchBody, Smoothness, SurfaceNormals and BodySpecular have no effect here.
// Controls: c0 = strength, specular, reflectivity, metallic; c1 = ignore-mask, energy-conserving, highlight ceiling, debug tint.
const char* kShader11 = R"(
samplerCUBE S0 : register(s0);
sampler2D S1 : register(s1);
float4 uA : register(c0);
float4 uB : register(c1);
struct PSIn {
  float4 col0 : COLOR0; float4 col1 : COLOR1;
  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
};
float4 main(PSIn i) : COLOR0 {
  float4 tex = tex2D(S1, i.t1.xy);
  float3 env = texCUBE(S0, i.t0.xyz).rgb;
  float d = saturate(dot(saturate(i.t2.xyz), saturate(i.t3.xyz)));
  float d2 = d * d;
  float d4 = d2 * d2;
  float sp = 2.0 * d4 * d4;
  float3 albedo = tex.rgb * i.col0.rgb;
  float3 maskS = i.col0.w * i.col1.rgb;
  float maskE = i.col0.w * i.col1.w;

  // The game's own result. Used when Strength is 0.
  float3 outO = albedo + sp * maskS + env * maskE;

  // Our result.
  float3 tint = lerp(float3(1, 1, 1), saturate(albedo), uA.w);
  float3 mS = lerp(maskS, float3(1, 1, 1), uB.x);
  float mE = lerp(maskE, 1.0, uB.x);
  float3 litP = tint * sp * uA.y * mS;
  if (uB.z < 0.0) litP = float3(0, 0, 0);                          // HighlightMax=0: no added highlight
  else if (uB.z > 0.0) litP = uB.z * (1.0 - exp(-litP / uB.z));    // HighlightMax=n: soft ceiling at n
  // The old shader reflects twice as strongly as the ps_3_0 one (which multiplies by 0.5), so Reflectivity is doubled here
  // to keep the same meaning.
  float3 envRaw = env * mE * 2.0;
  float3 base = albedo * (1.0 - uA.w);
  float3 outP = base + litP + envRaw * tint * uA.z;
  float share = saturate(uA.z * 2.0);
  float3 outE = base * (1.0 - share) + litP + envRaw * tint * share;
  outP = lerp(outP, outE, uB.y);

  float3 rgb = lerp(outO, outP, uA.x);
  rgb = lerp(rgb, float3(1, 0, 1), uB.w * 0.5);
  return float4(rgb, tex.a * i.col0.w);
}
)";
IDirect3DPixelShader9* g_ps11 = nullptr;
bool g_ps11Failed = false;

// ---- Draw identification --------------------------------------------------------------------------------------------------
// A draw is identified by what DrawIndexedPrimitive receives: the index buffer, the first index and the triangle count.
struct Key {
    uint32_t ib, start, prims;
    bool operator==(const Key& o) const { return ib == o.ib && start == o.start && prims == o.prims; }
};
struct KeyHash {
    size_t operator()(const Key& k) const { return (size_t(k.ib) * 31 + k.start) * 31 + k.prims; }
};
// The tables below are filled while the game queues its draws and are cleared at every Present (one frame).
std::unordered_map<Key, uint8_t, KeyHash> g_tags;       // decal draw -> kind
std::unordered_map<uint32_t, uint8_t> g_ibKinds;        // whole index buffer -> kind (used when the draw's range could not be read)
std::unordered_set<uint32_t> g_bodyIBs;                 // index buffers of body parts (the source of MatchBody values)
float g_bodyHot = 0, g_bodyPower = 1;                   // highlight hotspot and power of the body paint, seen on its last opaque draw
bool g_bodyValid = false;                               // a body draw has been seen this frame
std::unordered_map<IDirect3DPixelShader9*, int> g_carPsKind;  // cache: 3 = the game's ps_3_0 car shader, 1 = the ps_1_1 one, 0 = other
IDirect3DDevice9* g_lastDevice = nullptr;
uint64_t g_stat[4]{};                                   // replaced draws per Kind (reported in the log)
uint64_t g_ovTicks = 0, g_origTicks = 0, g_hookTicks = 0;     // time spent in this plug-in, see the log line every 600 frames
inline uint64_t qpc() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return static_cast<uint64_t>(v.QuadPart); }

// Memory reads that cannot crash: the addresses come from game structures that may be stale. (Checking each address with
// VirtualQuery was too slow, since this runs for every part of every frame.)
static bool tryRead32(uintptr_t a, uint32_t& out) {
    __try { out = *reinterpret_cast<const uint32_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool tryCopy(uintptr_t a, void* dst, size_t n) {
    __try { memcpy(dst, reinterpret_cast<const void*>(a), n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// What a car part is, from its name.
enum : uint8_t { kFlagDecal = 1, kFlagWindow = 2, kFlagBody = 4 };
// A "solid" is the game's model of one car part. Its header starts with the part name, such as "XXX_KIT00_BODY_A".
// classifySolid finds the first identifier of 8 or more characters that contains '_' and sorts the part by it:
// "DECAL" = decal (with "WINDOW" = window decal), "_BODY_" = body. Called once per solid; the result is cached.
std::unordered_map<uint32_t, std::string> g_partNames;   // solid pointer -> part name (only for the log)
thread_local uint32_t t_solid = 0;                        // the solid being rendered right now
uint8_t classifySolid(uintptr_t solid) {
    char b[0x101];
    if (!tryCopy(solid, b, 0x100)) return 0;
    b[0x100] = 0;
    auto isId = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; };
    for (size_t i = 0; i < 0x100;) {
        size_t j = i;
        while (j < 0x100 && isId(b[j])) ++j;
        if (j - i >= 8) {
            char n[0x101];
            memcpy(n, b + i, j - i);
            n[j - i] = 0;
            if (strchr(n, '_')) {
                for (char* p = n; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 32;
                uint8_t f = 0;
                if (strstr(n, "DECAL")) {
                    f |= kFlagDecal;
                    if (strstr(n, "WINDOW")) f |= kFlagWindow;
                } else if (strstr(n, "_BODY_")) {
                    f |= kFlagBody;
                }
                static int logged = 0; // log lines written: the first 400, or all with LogParts=1
                std::string& known = g_partNames[static_cast<uint32_t>(solid)];   // the class cache is cleared often, so log a name only when it is new
                const bool fresh = known != n;
                known = n;
                if (fresh && g_cfg.log && logged < (g_cfg.logParts ? 20000 : 400)) { ++logged; log(std::string("part ") + n + " flags=" + std::to_string(f)); }
                return f;
            }
        }
        i = j + 1;
    }
    return 0;
}
std::unordered_map<uint32_t, uint8_t> g_partCache; // solid pointer -> flags (cleared often: the game reuses pointers)
thread_local uint8_t t_flags = 0;                   // flags of the part being rendered right now (set around RenderPart)
using PartFn = void(__fastcall*)(void*, void*, void*, void*, void*, void*, uint32_t);
using SubmitFn = void(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
PartFn g_origPart{};
SubmitFn g_origSubmit{};

// Hook of CarRenderInfo::RenderPart. Notes which kind of part is being rendered while the original runs, so that the
// submit hook below knows what the draw it sees belongs to.
void __fastcall partHook(void* self, void* edx, void* view, void* part, void* world, void* light, uint32_t flags) {
    const uint8_t prev = t_flags;
    const uint32_t prevSolid = t_solid;
    t_flags = 0;
    t_solid = 0;
    if (g_cfg.enabled) {
        const uint64_t t = qpc();
        uint32_t m0 = 0, solid = 0;
        if (part && tryRead32(reinterpret_cast<uintptr_t>(part), m0)) {
            const uint32_t model = m0 & ~3u;
            if (model && tryRead32(model + 12, solid) && solid) {
                auto it = g_partCache.find(solid);
                if (it == g_partCache.end()) it = g_partCache.emplace(solid, classifySolid(solid)).first;
                t_flags = it->second;
                t_solid = solid;
            }
        }
        g_hookTicks += qpc() - t;
    }
    g_origPart(self, edx, view, part, world, light, flags);
    t_flags = prev;
    t_solid = prevSolid;
}

// LogParts: if the item just queued uses a texture whose name contains NUMBER, log it once per part with the part's flags.
// This shows number decals on parts that are not classified as decals (for example on a modded car).
std::unordered_set<uint32_t> g_numberLogged;
void logNumberTexture() {
    const uint32_t count = *reinterpret_cast<uint32_t*>(kQueueCount);
    const uint32_t entries = *reinterpret_cast<uint32_t*>(kQueueEntries);
    uint32_t idx = 0, it9 = 0;
    if (!count || count > 4096 || !entries || !tryRead32(entries + (count - 1) * 8, idx) || idx >= 4096) return;
    if (!tryRead32(kItems + 0x44 * size_t(idx) + 36, it9)) return;
    char tex[25]{};
    if (!tryCopy(it9 + 0xC, tex, 24) || !strstr(tex, "NUMBER")) return;
    if (!g_numberLogged.insert(t_solid).second) return;
    log("NUMBER texture '" + std::string(tex) + "' queued by part " + g_partNames[t_solid] + " flags=" + std::to_string(t_flags));
}

// Hook of the mesh submit function. After the original has queued a render item, this reads the item and records its draw
// (index buffer, first index, triangle count) as a decal or as a body part.
void __cdecl submitHook(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h, uintptr_t i) {
    g_origSubmit(a, b, c, d, e, f, g, h, i);
    if (g_cfg.logParts && g_cfg.enabled && t_solid) logNumberTexture();
    if (!t_flags || !g_cfg.enabled) return;
    const uint64_t t0 = qpc();
    struct Timer { uint64_t t; ~Timer() { g_hookTicks += qpc() - t; } } timer{t0};
    // The item just queued is the last entry of the render queue.
    const uint32_t count = *reinterpret_cast<uint32_t*>(kQueueCount);
    const uint32_t entries = *reinterpret_cast<uint32_t*>(kQueueEntries);
    uint32_t idx = 0;
    if (!count || count > 4096 || !entries || !tryRead32(entries + (count - 1) * 8, idx) || idx >= 4096) return;
    uint32_t it[17];
    if (!tryCopy(kItems + 0x44 * size_t(idx), it, sizeof it)) return;
    uint32_t m1 = 0, mesh[12];
    if (!tryRead32(it[2], m1) || !tryCopy(m1, mesh, sizeof mesh)) return;
    const uint32_t ib = mesh[10]; // the mesh's IDirect3DIndexBuffer9*
    if (t_flags & kFlagBody) { g_bodyIBs.insert(ib); return; }
    if (!(t_flags & kFlagDecal)) return;
    char tex[25]{};
    tryCopy(it[9] + 0xC, tex, 24);
    Kind kind = kDecal;
    if (strstr(tex, "NUMBER")) kind = kNumber;
    else if (t_flags & kFlagWindow) kind = kWindow;
    uint32_t strip[18];
    if (tryCopy(it[0], strip, sizeof strip) && strip[16] > 0 && strip[16] < 100000 && strip[17] < 4000000) {
        g_tags[{ib, strip[17], strip[16]}] = kind;
    } else {
        g_ibKinds[ib] = kind; // the range could not be read: mark the whole buffer
    }
}

// ---- Direct3D 9 hooks ----------------------------------------------------------------------------------------------------
using DipFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
DipFn g_origDip{};
PresentFn g_origPresent{};

// FNV-1a hash of a byte range.
uint32_t fnv(const void* data, size_t size) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) h = (h ^ static_cast<const uint8_t*>(data)[i]) * 16777619u;
    return h;
}
// Which of the game's car pixel shaders `ps` is, found by the hash of its bytecode: 3 = ps_3_0, 1 = ps_1_1, 0 = something else.
// The answer is cached per shader object.
int carPsKind(IDirect3DPixelShader9* ps) {
    auto it = g_carPsKind.find(ps);
    if (it != g_carPsKind.end()) return it->second;
    int kind = 0;
    UINT size = 0;
    if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size && size < (1u << 20)) {
        std::string code(size, '\0');
        if (SUCCEEDED(ps->GetFunction(code.data(), &size))) {
            const uint32_t h = fnv(code.data(), size);
            kind = h == kCarPixelShaderHash ? 3 : (h == kCarPixelShaderHash11 ? 1 : 0);
        }
    }
    g_carPsKind[ps] = kind;
    return kind;
}
// True for the ps_3_0 car shader of mods like REDUX (the only one the normal pre-pass and the full controls work with).
bool isCarPs(IDirect3DPixelShader9* ps) { return carPsKind(ps) == 3; }
// A body or decal draw that does not use the known car pixel shader (another paint type, a shader mod): its hash is logged and
// its disassembly is written next to the log, once per shader. This shows what would have to be supported.
std::unordered_set<uint32_t> g_dumpedOther;
void dumpOtherPs(IDirect3DPixelShader9* ps, const char* what) {
    UINT size = 0;
    if (FAILED(ps->GetFunction(nullptr, &size)) || !size || size > (1u << 20)) return;
    std::string code(size, '\0');
    if (FAILED(ps->GetFunction(code.data(), &size))) return;
    const uint32_t h = fnv(code.data(), size);
    if (!g_dumpedOther.insert(h).second) return;
    char name[64];
    snprintf(name, sizeof name, "NFSMWHQDecals_other_ps_%08X.txt", h);
    ID3DBlob* blob = nullptr;
    if (SUCCEEDED(D3DDisassemble(code.data(), size, 0, nullptr, &blob)) && blob) {
        std::ofstream(g_dir / name, std::ios::binary).write(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
        blob->Release();
    }
    log(std::string("other pixel shader used by a ") + what + " draw: " + name);
}
// Compiles the replacement pixel shader on first use. If that fails, it is not tried again.
bool buildShader(IDirect3DDevice9* d) {
    if (g_ps) return true;
    if (g_psFailed) return false;
    ID3DBlob *code = nullptr, *err = nullptr;
    const HRESULT hr = D3DCompile(kShader, strlen(kShader), "hqdecals.hlsl", nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &err);
    if (FAILED(hr) || !code) {
        log(std::string("shader compile failed: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
        if (err) err->Release();
        g_psFailed = true;
        return false;
    }
    if (FAILED(d->CreatePixelShader(static_cast<DWORD*>(code->GetBufferPointer()), &g_ps))) g_psFailed = true;
    code->Release();
    if (err) err->Release();
    log(g_ps ? "replacement pixel shader created" : "CreatePixelShader failed");
    return g_ps != nullptr;
}

// Compiles the ps_1_1 replacement on first use. If that fails, it is not tried again.
bool buildShader11(IDirect3DDevice9* d) {
    if (g_ps11) return true;
    if (g_ps11Failed) return false;
    ID3DBlob *code = nullptr, *err = nullptr;
    const HRESULT hr = D3DCompile(kShader11, strlen(kShader11), "hqdecals11.hlsl", nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &err);
    if (FAILED(hr) || !code) {
        log(std::string("ps_1_1 replacement compile failed: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
        if (err) err->Release();
        g_ps11Failed = true;
        return false;
    }
    if (FAILED(d->CreatePixelShader(static_cast<DWORD*>(code->GetBufferPointer()), &g_ps11))) g_ps11Failed = true;
    code->Release();
    if (err) err->Release();
    log(g_ps11 ? "ps_1_1 replacement pixel shader created" : "CreatePixelShader failed (ps_1_1 replacement)");
    return g_ps11 != nullptr;
}

// Draws one decal with the ps_1_1 replacement (see kShader11).
static HRESULT drawDecal11(IDirect3DDevice9* d, IDirect3DPixelShader9* ps, const Group& g, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIdx, UINT numVerts, UINT start, UINT prims);
// Calls the game's DrawIndexedPrimitive and adds its time to g_origTicks, so that the time spent in the game's own draw is
// not counted as this plug-in's cost.
HRESULT callOrig(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIdx, UINT numVerts, UINT start, UINT prims) {
    const uint64_t t = qpc();
    const HRESULT hr = g_origDip(d, type, baseVertex, minIdx, numVerts, start, prims);
    g_origTicks += qpc() - t;
    return hr;
}
// ---- Normal pre-pass: the body's own normals, per pixel ---------------------------------------------------------------------------------
// Every opaque car draw is drawn once more, right after the game's own draw, into an extra render target. The target holds the
// normal in local space as "normal * 0.5 + 0.5" and, in alpha, a marker and the body's highlight strength (see kNormalShader).
// The draw uses the game's shaders, matrices and depth buffer, with depth test LESS_EQUAL and no depth write, so only the visible
// surface is written. A decal pixel then reads the target at its own screen position and finds the body that lies under it.
// The decal and the body share one local space (their LocalDirectionMatrix is the same), so the normal can be used directly.
const char* kNormalShader = R"(
struct PSIn {
  float4 col0 : COLOR0; float4 col1 : COLOR1;
  float3 envDir : TEXCOORD0; float2 uv : TEXCOORD1; float4 shadow : TEXCOORD2; float4 light : TEXCOORD3;
  float3 nrm : TEXCOORD4; float3 view : TEXCOORD5; float3 vol : TEXCOORD6;
};
// Alpha: 0 = no body here; otherwise 0.5 + 0.25 * s. s is the strength with which the body's own shader adds its highlight at
// this pixel. (The car shader multiplies the highlight by COLOR0.w and COLOR1.xyz: the vertex-colour mask and the material.)
float4 main(PSIn i) : COLOR0 {
  const float s = min(i.col0.w * dot(i.col1.xyz, float3(1.0, 1.0, 1.0)) * (1.0 / 3.0), 2.0);
  return float4(normalize(i.nrm) * 0.5 + 0.5, 0.5 + 0.25 * s);
}
)";
IDirect3DPixelShader9* g_nrmPS = nullptr;
bool g_nrmPsFailed = false;
// One normal target per render target size (the game draws the scene and its reflections at different sizes).
struct NrmTarget { IDirect3DTexture9* tex = nullptr; IDirect3DSurface9* surf = nullptr; UINT w = 0, h = 0; uint64_t clearedFrame = ~0ull; };
std::map<uint64_t, NrmTarget> g_nrmTargets;   // key: width << 32 | height
uint64_t g_frameNo = 0;                       // counts Present calls; a target is cleared on its first use in a frame
bool g_prepassPending = false;                // dipImpl sets it for an opaque car draw; dipHook then runs the pre-pass after the game's draw
// Frees the normal targets and shader (used when the device is recreated).
void releaseNrm() {
    for (auto& kv : g_nrmTargets) { if (kv.second.surf) kv.second.surf->Release(); if (kv.second.tex) kv.second.tex->Release(); }
    g_nrmTargets.clear();
    if (g_nrmPS) { g_nrmPS->Release(); g_nrmPS = nullptr; }
    g_nrmPsFailed = false;
}
// Compiles the pre-pass shader on first use. If that fails, it is not tried again.
static bool buildNrmShader(IDirect3DDevice9* d) {
    if (g_nrmPS) return true;
    if (g_nrmPsFailed) return false;
    ID3DBlob *code = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kNormalShader, strlen(kNormalShader), "nrm.hlsl", nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &err)) || !code) {
        log(std::string("normal shader compile failed: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
        if (err) err->Release();
        g_nrmPsFailed = true;
        return false;
    }
    if (FAILED(d->CreatePixelShader(static_cast<DWORD*>(code->GetBufferPointer()), &g_nrmPS))) g_nrmPsFailed = true;
    code->Release();
    if (err) err->Release();
    if (g_nrmPS) log("normal pre-pass shader created");
    return g_nrmPS != nullptr;
}
// True if the current draw uses the car pixel shader without alpha blending, which is how the body is drawn.
static bool isCarOpaque(IDirect3DDevice9* d) {
    IDirect3DPixelShader9* ps = nullptr;
    d->GetPixelShader(&ps);
    DWORD blend = 0;
    d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    const bool car = ps && !blend && isCarPs(ps);
    if (ps) ps->Release();
    return car;
}
// Draws the current draw once more into the normal target that matches the current render target's size. All the render state
// it changes is put back afterwards.
static void normalPrepass(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIdx, UINT numVerts, UINT start, UINT prims) {
    if (!buildNrmShader(d)) return;
    IDirect3DSurface9* oldRT = nullptr;
    if (FAILED(d->GetRenderTarget(0, &oldRT)) || !oldRT) return;
    D3DSURFACE_DESC rd{};
    oldRT->GetDesc(&rd);
    IDirect3DSurface9* ds = nullptr;
    D3DSURFACE_DESC dd{};
    const bool haveDs = SUCCEEDED(d->GetDepthStencilSurface(&ds)) && ds && SUCCEEDED(ds->GetDesc(&dd));
    if (ds) ds->Release();
    // Needs a render target without multisampling, with a depth buffer of the same kind that is at least as large.
    if (!haveDs || rd.MultiSampleType != D3DMULTISAMPLE_NONE || dd.MultiSampleType != D3DMULTISAMPLE_NONE || !(rd.Usage & D3DUSAGE_RENDERTARGET) ||
        rd.Width < 64 || rd.Height < 64 || dd.Width < rd.Width || dd.Height < rd.Height) {
        static bool said = false;
        if (!said) { said = true; char s[200]; snprintf(s, sizeof s, "normal pre-pass: render target not usable (%ux%u ms=%d usage=%u, depth %ux%u ms=%d have=%d)", rd.Width, rd.Height, int(rd.MultiSampleType), rd.Usage, dd.Width, dd.Height, int(dd.MultiSampleType), int(haveDs)); log(s); }
        oldRT->Release();
        return;
    }
    NrmTarget& t = g_nrmTargets[(uint64_t(rd.Width) << 32) | rd.Height];
    if (!t.tex) {
        if (FAILED(d->CreateTexture(rd.Width, rd.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t.tex, nullptr)) || !t.tex ||
            FAILED(t.tex->GetSurfaceLevel(0, &t.surf)) || !t.surf) {
            log("normal pre-pass: could not create the target");
            if (t.tex) { t.tex->Release(); t.tex = nullptr; }
            oldRT->Release();
            return;
        }
        t.w = rd.Width; t.h = rd.Height;
        char s[120]; snprintf(s, sizeof s, "normal pre-pass: target %ux%u created", t.w, t.h); log(s);
    }
    D3DVIEWPORT9 vp{};
    d->GetViewport(&vp);
    IDirect3DPixelShader9* oldPs = nullptr;
    d->GetPixelShader(&oldPs);
    const D3DRENDERSTATETYPE rs[] = {D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_COLORWRITEENABLE,
                                     D3DRS_SRGBWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_STENCILENABLE};
    DWORD saved[8]{};
    for (int i = 0; i < 8; ++i) d->GetRenderState(rs[i], &saved[i]);
    d->SetRenderTarget(0, t.surf);
    if (t.clearedFrame != g_frameNo) { // first car draw of this frame at this size: clear it (alpha 0 = no body)
        d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        d->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 128, 128, 128), 1.0f, 0);
        t.clearedFrame = g_frameNo;
    }
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetViewport(&vp);
    d->SetPixelShader(g_nrmPS);
    g_origDip(d, type, baseVertex, minIdx, numVerts, start, prims);
    d->SetPixelShader(oldPs);
    for (int i = 0; i < 8; ++i) d->SetRenderState(rs[i], saved[i]);
    d->SetRenderTarget(0, oldRT);
    d->SetViewport(&vp);
    if (oldPs) oldPs->Release();
    oldRT->Release();
}
// The normal target for the current render target's size, if a pre-pass has drawn into it this frame (otherwise nullptr).
static NrmTarget* currentNrm(IDirect3DDevice9* d) {
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return nullptr;
    D3DSURFACE_DESC rd{};
    rt->GetDesc(&rd);
    rt->Release();
    const auto it = g_nrmTargets.find((uint64_t(rd.Width) << 32) | rd.Height);
    return (it != g_nrmTargets.end() && it->second.tex && it->second.clearedFrame == g_frameNo) ? &it->second : nullptr;
}
HRESULT dipImpl(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIdx, UINT numVerts, UINT start, UINT prims);
// Hook of DrawIndexedPrimitive. dipImpl does the work; this adds the normal pre-pass after an opaque car draw and measures the cost.
HRESULT __stdcall dipHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIdx, UINT numVerts, UINT start, UINT prims) {
    const uint64_t t0 = qpc(), o0 = g_origTicks;
    g_prepassPending = false;
    const HRESULT hr = dipImpl(d, type, baseVertex, minIdx, numVerts, start, prims);
    if (g_prepassPending) { // an opaque car draw while decals are queued: write its normals into the extra target
        g_prepassPending = false;
        normalPrepass(d, type, baseVertex, minIdx, numVerts, start, prims);
    }
    g_ovTicks += (qpc() - t0) - (g_origTicks - o0);
    return hr;
}
// DebugNormals: draws with the replacement shader showing normals as colours, with neutral constants.
static HRESULT drawNormalColours(IDirect3DDevice9* d, IDirect3DPixelShader9* ps, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIdx, UINT numVerts, UINT start, UINT prims) {
    const float uni[12] = {0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0};
    float old[12]{};
    d->GetPixelShaderConstantF(20, old, 3);
    d->SetPixelShaderConstantF(20, uni, 3);
    d->SetPixelShader(g_ps);
    const HRESULT hr = callOrig(d, type, baseVertex, minIdx, numVerts, start, prims);
    d->SetPixelShader(ps);
    d->SetPixelShaderConstantF(20, old, 3);
    return hr;
}
// Draws one decal with the ps_1_1 replacement. The controls go to c0 and c1 (the old shader uses no constants, so they are free).
static HRESULT drawDecal11(IDirect3DDevice9* d, IDirect3DPixelShader9* ps, const Group& g, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIdx, UINT numVerts, UINT start, UINT prims) {
    if (!g.enabled || g.strength <= 0.0f || !buildShader11(d)) return callOrig(d, type, baseVertex, minIdx, numVerts, start, prims);
    const float ceiling = g.highlightMax <= 0.0f ? -1.0f : (std::isinf(g.highlightMax) ? 0.0f : g.highlightMax);   // <0 none, 0 no ceiling, >0 ceiling
    const float uni[8] = {g.strength, g.specular, g.reflectivity, g.metallic, g.ignoreMask, g.energy ? 1.0f : 0.0f, ceiling, g_cfg.debug ? 1.0f : 0.0f};
    float old[8]{};
    d->GetPixelShaderConstantF(0, old, 2);
    d->SetPixelShaderConstantF(0, uni, 2);
    d->SetPixelShader(g_ps11);
    const HRESULT hr = callOrig(d, type, baseVertex, minIdx, numVerts, start, prims);
    d->SetPixelShader(ps);
    d->SetPixelShaderConstantF(0, old, 2);
    return hr;
}
// The decision for one draw: decal (replace the shader), body (note its highlight values), or anything else (pass through).
HRESULT dipImpl(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIdx, UINT numVerts, UINT start, UINT prims) {
    if (!g_cfg.enabled || (!g_cfg.debugNormals && g_tags.empty() && g_ibKinds.empty() && g_bodyIBs.empty()))
        return callOrig(d, type, baseVertex, minIdx, numVerts, start, prims);
    IDirect3DIndexBuffer9* ib = nullptr;
    d->GetIndices(&ib);
    const uint32_t ibp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ib));
    if (ib) ib->Release();
    uint8_t kind = 0;
    if (auto it = g_tags.find({ibp, start, prims}); it != g_tags.end()) kind = it->second;
    else if (auto it2 = g_ibKinds.find(ibp); it2 != g_ibKinds.end()) kind = it2->second;
    const bool isBody = !kind && g_bodyIBs.count(ibp);
    if (!kind && g_cfg.surfaceNormals && (!g_tags.empty() || !g_ibKinds.empty())) g_prepassPending = isCarOpaque(d);   // decals are queued, so the body's normals are needed
    if (!kind && !isBody) {
        if (g_cfg.debugNormals) { // also show every other opaque car draw as normals (a body shaped by another mod uses its own buffers)
            IDirect3DPixelShader9* p2 = nullptr;
            d->GetPixelShader(&p2);
            DWORD blend = 0;
            d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
            if (p2 && !blend && isCarPs(p2) && buildShader(d)) {
                const HRESULT hr = drawNormalColours(d, p2, type, baseVertex, minIdx, numVerts, start, prims);
                p2->Release();
                return hr;
            }
            if (p2) p2->Release();
        }
        return callOrig(d, type, baseVertex, minIdx, numVerts, start, prims);
    }

    IDirect3DPixelShader9* ps = nullptr;
    d->GetPixelShader(&ps);
    if (ps && carPsKind(ps) == 1) { // the old ps_1_1 car shader: a decal gets the reduced replacement, the body is left alone
        HRESULT hr;
        if (kind) {
            hr = drawDecal11(d, ps, kind == kNumber ? g_cfg.number : g_cfg.bodyWindow, type, baseVertex, minIdx, numVerts, start, prims);
            ++g_stat[kind];
        } else {
            hr = callOrig(d, type, baseVertex, minIdx, numVerts, start, prims);
        }
        ps->Release();
        return hr;
    }
    if (!ps || !isCarPs(ps)) {
        if (ps && g_cfg.log) dumpOtherPs(ps, kind ? "decal" : "body"); // log which shader is used instead
        if (ps) ps->Release();
        return callOrig(d, type, baseVertex, minIdx, numVerts, start, prims);
    }
    float c34[8]{};
    d->GetPixelShaderConstantF(3, c34, 2);
    if (isBody) { // keep the body paint's highlight values for MatchBody (opaque body draws come before the blended decals)
        DWORD blend = 0;
        d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
        if (!blend) {
            // A power below 1 is not paint (power 0 would make the highlight cover the whole surface): keep the previous values.
            if (c34[4] >= 1.0f) { g_bodyHot = c34[0]; g_bodyPower = c34[4]; g_bodyValid = true; }
        }
        if (g_cfg.debugNormals && !blend && buildShader(d)) { // DebugNormals: the opaque body as normal colours too
            const HRESULT hr = drawNormalColours(d, ps, type, baseVertex, minIdx, numVerts, start, prims);
            ps->Release();
            return hr;
        }
        ps->Release();
        return callOrig(d, type, baseVertex, minIdx, numVerts, start, prims);
    }
    const Group& g = (kind == kNumber) ? g_cfg.number : g_cfg.bodyWindow;
    if (!g.enabled || g.strength <= 0.0f || !buildShader(d)) {
        ps->Release();
        return callOrig(d, type, baseVertex, minIdx, numVerts, start, prims);
    }
    // hot = strength of the sharp highlight spot, power = how tight the highlight is.
    // With the compensation, the highlight is made to look as it does with the game's own (ps_1_1) shader: the shape is
    // 2 * d^8, there is no sharp spot, no Fresnel, and the strength does not follow the body's paint. Specular scales all of it.
    // Without it, MatchBody, Smoothness, Fresnel and BodySpecular work as they are.
    const bool comp = g_cfg.compensate;
    float specScale = comp ? 2.0f * g.specular : 1.0f;
    float hot, power;
    if (comp) {
        hot = 0.0f;
        power = 8.0f;
    } else if (g.matchBody && g_bodyValid) {
        hot = g_bodyHot * g.specular;
        power = g_bodyPower;
    } else {
        hot = g.specular;
        const float sm = g.smoothness;
        power = 0.2f + (8.0f - 0.2f) * sm * sm;
    }
    // The normal target of this frame for the current render target, if there is one.
    NrmTarget* nrm = g_cfg.surfaceNormals ? currentNrm(d) : nullptr;
    // The controls for the replacement shader: c20-c23, four floats each (the layout is described above kShader).
    float uni[16] = {g.strength, hot, power, g.reflectivity, g.metallic, comp ? 0.0f : g.fresnel, g_cfg.debug ? 1.0f : 0.0f, g.ignoreMask,
                     g.energy ? 1.0f : 0.0f, g_cfg.debugNormals ? 1.0f : 0.0f, (nrm && g_cfg.bodySpecular && !comp) ? 1.0f : 0.0f, g.highlightMax <= 0.0f ? -1.0f : (std::isinf(g.highlightMax) ? 0.0f : g.highlightMax),   // shader: <0 none, 0 no ceiling, >0 ceiling
                     nrm ? 1.0f / nrm->w : 0.0f, nrm ? 1.0f / nrm->h : 0.0f, nrm ? 1.0f : 0.0f, (nrm && g_cfg.debugNormals) ? 1.0f : 0.0f};
    float old[16]{};
    d->GetPixelShaderConstantF(20, old, 4);
    d->SetPixelShaderConstantF(20, uni, 4);
    const float scale4[4] = {specScale, 0.0f, 0.0f, 0.0f};
    float oldF[4]{};
    d->GetPixelShaderConstantF(27, oldF, 1);
    d->SetPixelShaderConstantF(27, scale4, 1);
    // c24-c26: WorldView rows, for the cube map direction.
    float oldE[12]{}, rows[12]{};
    d->GetPixelShaderConstantF(24, oldE, 3);
    if (nrm && SUCCEEDED(d->GetVertexShaderConstantF(8, rows, 3))) d->SetPixelShaderConstantF(24, rows, 3);
    IDirect3DBaseTexture9* prevTex = nullptr;
    DWORD prevSamp[5]{};
    const D3DSAMPLERSTATETYPE ss[5] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV};
    if (nrm) { // the car shader does not use texture stage 4: put the normal target there, read per pixel without filtering
        d->GetTexture(4, &prevTex);
        for (int k = 0; k < 5; ++k) d->GetSamplerState(4, ss[k], &prevSamp[k]);
        d->SetTexture(4, nrm->tex);
        d->SetSamplerState(4, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(4, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(4, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(4, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d->SetSamplerState(4, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    d->SetPixelShader(g_ps);
    const HRESULT hr = callOrig(d, type, baseVertex, minIdx, numVerts, start, prims);
    // Put everything back as the game had it.
    d->SetPixelShader(ps);
    d->SetPixelShaderConstantF(20, old, 4);
    d->SetPixelShaderConstantF(24, oldE, 3);
    d->SetPixelShaderConstantF(27, oldF, 1);
    if (nrm) {
        d->SetTexture(4, prevTex);
        for (int k = 0; k < 5; ++k) d->SetSamplerState(4, ss[k], prevSamp[k]);
    }
    if (prevTex) prevTex->Release();
    ps->Release();
    ++g_stat[kind];
    return hr;
}

// Hook of Present: once per frame. Handles device changes, the periodic log line, HotReload, and clears the per-frame tables.
HRESULT __stdcall presentHook(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    static uint64_t frame = 0;
    ++g_frameNo;
    if (d != g_lastDevice) { // the device was (re)created: cached shaders and render targets belong to the old one
        g_lastDevice = d;
        g_carPsKind.clear();
        releaseNrm();
        if (g_ps) { g_ps->Release(); g_ps = nullptr; }
        if (g_ps11) { g_ps11->Release(); g_ps11 = nullptr; }
        g_ps11Failed = false;
        g_psFailed = false;
    }
    static uint64_t lastLogTick = qpc();
    if (++frame % 600 == 0) {
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        const uint64_t now2 = qpc();
        const double ms = 1000.0 / double(fq.QuadPart);
        char line[300];
        // frameMs = average frame time over the last 600 frames. modMs = this plug-in's own time per frame (draw hook + part/submit hooks).
        snprintf(line, sizeof line,
                 "frames=%llu replaced draws: number=%llu decal=%llu window=%llu tags=%zu | frameMs=%.2f modMs=%.3f (draw %.3f, part/submit %.3f)",
                 (unsigned long long)frame, (unsigned long long)g_stat[1], (unsigned long long)g_stat[2], (unsigned long long)g_stat[3],
                 g_tags.size(), double(now2 - lastLogTick) * ms / 600.0, double(g_ovTicks + g_hookTicks) * ms / 600.0,
                 double(g_ovTicks) * ms / 600.0, double(g_hookTicks) * ms / 600.0);
        log(line);
        g_partCache.clear(); // solid pointers can be reused when cars are loaded/unloaded
        lastLogTick = now2;
        g_ovTicks = g_hookTicks = g_origTicks = 0;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_cfg.hotReload && now >= g_nextIniCheck) { g_nextIniCheck = now + 1000; loadIni(); }
    // Render items are queued and drawn within one frame. Forget them, so a buffer pointer that the game reuses never keeps an old tag.
    g_tags.clear();
    g_ibKinds.clear();
    g_bodyIBs.clear();
    g_partCache.clear();
    g_bodyValid = false;
    return g_origPresent(d, a, b, w, r);
}

// Runs in its own thread after the ASI is loaded: waits for the game's Direct3D device, then installs the hooks.
DWORD WINAPI worker(LPVOID) {
    IDirect3DDevice9* dev = nullptr;
    for (int i = 0; i < 1200 && !dev; ++i) { // wait up to about 2 minutes for the game to create its device
        dev = *reinterpret_cast<IDirect3DDevice9**>(kDevicePtr);
        if (!dev) Sleep(100);
    }
    if (!dev) return 0;
    Sleep(2000);   // give the game time to finish setting up before hooking
    dev = *reinterpret_cast<IDirect3DDevice9**>(kDevicePtr);
    if (!dev || MH_Initialize() != MH_OK) return 0;
    void** table = *reinterpret_cast<void***>(dev);
    // Hook the functions inside d3d9.dll (index 82 = DrawIndexedPrimitive, 17 = Present), not the device's vtable slots,
    // because other mods overwrite those slots.
    MH_CreateHook(table[82], reinterpret_cast<void*>(&dipHook), reinterpret_cast<void**>(&g_origDip));
    MH_CreateHook(table[17], reinterpret_cast<void*>(&presentHook), reinterpret_cast<void**>(&g_origPresent));
    MH_CreateHook(reinterpret_cast<void*>(0x7387E0), reinterpret_cast<void*>(&partHook), reinterpret_cast<void**>(&g_origPart));
    MH_CreateHook(reinterpret_cast<void*>(0x6E0AB0), reinterpret_cast<void*>(&submitHook), reinterpret_cast<void**>(&g_origSubmit));
    const MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
    log(st == MH_OK ? "hooks installed" : "MH_EnableHook failed");
    return 0;
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        g_dir = std::filesystem::path(path).parent_path();
        g_ini = g_dir / L"NFSMWHQDecals.ini";
        g_logPath = g_dir / L"NFSMWHQDecals.log";
        loadIni(true);
        if (g_cfg.log) std::ofstream(g_logPath, std::ios::trunc) << "NFSMWHQDecals started\n";   // with Log=0 no log file is written
        CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    }
    return TRUE;
}
