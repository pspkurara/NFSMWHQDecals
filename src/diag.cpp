// NFSMWHQDecalsDiag: discovery ASI. Logs one frame of D3D9 draws and can paint a chosen draw red,
// to find which draws are the number-plate / body-window decals and what shader they use.
//   F9  : capture the next frame to scripts/HQDecalsDiag/frame.log (+ shader disassembly files)
//   F7/F6 : highlight draw index +1 / -1 (painted solid red, original draw skipped); F5 : off
// The highlight index is relative to the draws of a frame, in submission order.
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "MinHook.h"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr uintptr_t kDevicePtr = 0x982BDC; // IDirect3DDevice9* in speed.exe 1.3

std::filesystem::path g_dir;
bool g_capture = false, g_capturing = false;
int g_highlight = -1;
int g_drawIndex = 0;
std::ostringstream g_log;
std::set<uint32_t> g_dumped;
IDirect3DPixelShader9* g_red = nullptr;

using DipFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
DipFn g_origDip{};
PresentFn g_origPresent{};

uint32_t crc(const void* data, size_t size) {
    uint32_t h = 2166136261u; // FNV-1a is enough to tell shaders apart
    for (size_t i = 0; i < size; ++i)
        h = (h ^ static_cast<const uint8_t*>(data)[i]) * 16777619u;
    return h;
}

template <class S>
uint32_t shaderHash(S* shader, const char* kind) {
    if (!shader)
        return 0;
    UINT size = 0;
    if (FAILED(shader->GetFunction(nullptr, &size)) || !size)
        return 0;
    std::vector<char> code(size);
    shader->GetFunction(code.data(), &size);
    const uint32_t h = crc(code.data(), size);
    if (g_dumped.insert(h ^ (kind[0] << 24)).second) {
        ID3DBlob* blob = nullptr;
        if (SUCCEEDED(D3DDisassemble(code.data(), size, 0, nullptr, &blob)) && blob) {
            char name[64];
            snprintf(name, sizeof name, "%s-%08X.txt", kind, h);
            std::ofstream(g_dir / name, std::ios::binary).write(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
            blob->Release();
        }
    }
    return h;
}

bool readable(const void* p, size_t n);
// The game's current effect wrapper is at 0x982C80 (set by the render-queue executor); its +0x48 is the ID3DXEffect.
// Minimal COM calls (d3dx9 headers not needed): GetTechniqueDesc = vtable[5], GetCurrentTechnique = vtable[59].
std::string currentTechnique(std::string* effectPtr = nullptr) {
    const uint32_t wrapper = *reinterpret_cast<uint32_t*>(0x982C80);
    if (!readable(reinterpret_cast<void*>(static_cast<uintptr_t>(wrapper)), 0x50)) return "no-effect";
    const uint32_t fx = *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(wrapper) + 0x48);
    if (effectPtr) { char b[16]; snprintf(b, sizeof b, "%08X", wrapper); *effectPtr = b; }
    if (!readable(reinterpret_cast<void*>(static_cast<uintptr_t>(fx)), 4)) return "no-fx";
    void** vt = *reinterpret_cast<void***>(static_cast<uintptr_t>(fx));
    if (!readable(vt, 60 * sizeof(void*))) return "no-vtable";
    using GetCurFn = const char*(__stdcall*)(void*);
    using DescFn = HRESULT(__stdcall*)(void*, const char*, void*);
    const char* handle = reinterpret_cast<GetCurFn>(vt[59])(reinterpret_cast<void*>(static_cast<uintptr_t>(fx)));
    if (!handle) return "no-tech";
    struct { const char* Name; UINT Passes; UINT Annotations; } desc{};
    if (FAILED(reinterpret_cast<DescFn>(vt[5])(reinterpret_cast<void*>(static_cast<uintptr_t>(fx)), handle, &desc)) || !desc.Name) return "no-desc";
    return readable(desc.Name, 4) ? std::string(desc.Name, strnlen(desc.Name, 48)) : "bad-name";
}

void describe(IDirect3DDevice9* d,D3DPRIMITIVETYPE type, INT baseVertex, UINT minIdx, UINT numVerts, UINT start, UINT prims) {
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    d->GetPixelShader(&ps);
    d->GetVertexShader(&vs);
    g_log << '#' << g_drawIndex << " type=" << type << " prims=" << prims << " verts=" << numVerts << " base=" << baseVertex
          << " ps=" << std::hex << shaderHash(ps, "ps") << " vs=" << shaderHash(vs, "vs") << std::dec;
    {
        std::string fxp;
        const std::string tech = currentTechnique(&fxp);
        g_log << " fx=" << fxp << " tech=" << tech;
    }
    DWORD ab = 0, zw = 0, at = 0, src = 0, dst = 0, cull = 0;
    d->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
    d->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
    d->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
    d->GetRenderState(D3DRS_SRCBLEND, &src);
    d->GetRenderState(D3DRS_DESTBLEND, &dst);
    d->GetRenderState(D3DRS_CULLMODE, &cull);
    g_log << " ab=" << ab << " src=" << src << " dst=" << dst << " zw=" << zw << " at=" << at << " cull=" << cull;
    for (DWORD s = 0; s < 8; ++s) {
        IDirect3DBaseTexture9* t = nullptr;
        if (FAILED(d->GetTexture(s, &t)) || !t)
            continue;
        g_log << " [s" << s << ' ' << std::hex << (reinterpret_cast<uintptr_t>(t) & 0xFFFFFFF) << std::dec;
        if (t->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC desc{};
            static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &desc);
            g_log << " 2D " << desc.Width << 'x' << desc.Height << " fmt" << desc.Format;
        } else if (t->GetType() == D3DRTYPE_CUBETEXTURE) {
            D3DSURFACE_DESC desc{};
            static_cast<IDirect3DCubeTexture9*>(t)->GetLevelDesc(0, &desc);
            g_log << " CUBE " << desc.Width << " fmt" << desc.Format;
        }
        g_log << ']';
        t->Release();
    }
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (SUCCEEDED(d->GetVertexDeclaration(&decl)) && decl) {
        D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH]{};
        UINT n = MAXD3DDECLLENGTH;
        decl->GetDeclaration(el, &n);
        g_log << " decl:";
        for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i)
            g_log << ' ' << int(el[i].Stream) << ':' << int(el[i].Offset) << ":t" << int(el[i].Type) << ":u" << int(el[i].Usage) << '.' << int(el[i].UsageIndex);
        decl->Release();
    }
    {
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT off = 0, stride = 0;
        IDirect3DIndexBuffer9* ib = nullptr;
        if (SUCCEEDED(d->GetStreamSource(0, &vb, &off, &stride)) && vb) {
            g_log << " vb=" << std::hex << (reinterpret_cast<uintptr_t>(vb) & 0xFFFFFFF) << std::dec << "+" << off << " stride=" << stride;
            vb->Release();
        }
        if (SUCCEEDED(d->GetIndices(&ib)) && ib) {
            g_log << " ib=" << std::hex << (reinterpret_cast<uintptr_t>(ib) & 0xFFFFFFF) << std::dec;
            ib->Release();
        }
        g_log << " minIdx=" << minIdx << " start=" << start;
    }
    float c[4]{};
    g_log << " ps_c0-7:";
    for (UINT i = 0; i < 8; ++i)
        if (SUCCEEDED(d->GetPixelShaderConstantF(i, c, 1)))
            g_log << " (" << c[0] << ',' << c[1] << ',' << c[2] << ',' << c[3] << ')';
    g_log << '\n';
    if (ps) ps->Release();
    if (vs) vs->Release();
}

// ---- Part / submission tagging: which car part each queued mesh belongs to --------------------------------------------
bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}
std::string solidName(const void* solid) { // first "A_B"-style identifier of 8+ chars in the solid header (same as VRM)
    if (!readable(solid, 0x100)) return {};
    const char* b = static_cast<const char*>(solid);
    for (size_t i = 0; i < 0x100;) {
        size_t j = i;
        while (j < 0x100 && (isalnum(static_cast<unsigned char>(b[j])) || b[j] == '_')) ++j;
        if (j - i >= 8) {
            std::string n(b + i, j - i);
            if (n.find('_') != std::string::npos) return n;
        }
        i = j + 1;
    }
    return {};
}
thread_local std::string t_part; // the part being rendered right now (set around CarRenderInfo::RenderPart)
using PartFn = void(__fastcall*)(void*, void*, void*, void*, void*, void*, uint32_t);
using SubmitFn = void(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
PartFn g_origPart{};
SubmitFn g_origSubmit{};

void __fastcall partHook(void* self, void* edx, void* view, void* part, void* world, void* light, uint32_t flags) {
    std::string prev = t_part;
    t_part.clear();
    if (g_capturing && readable(part, 4)) {
        const uintptr_t model = *reinterpret_cast<uintptr_t*>(part) & ~uintptr_t(3);
        if (model && readable(reinterpret_cast<void*>(model + 12), 4))
            t_part = solidName(*reinterpret_cast<void**>(model + 12));
    }
    g_origPart(self, edx, view, part, world, light, flags);
    t_part = prev;
}
void __cdecl submitHook(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h, uintptr_t i) {
    g_origSubmit(a, b, c, d, e, f, g, h, i);
    if (!g_capturing) return;
    // The entry just queued: its render item (0x44 bytes in the static array at 0x93E878).
    const uint32_t count = *reinterpret_cast<uint32_t*>(0x982CDC);
    auto* entries = *reinterpret_cast<uint32_t**>(0x9877B4);
    g_log << "SUBMIT part=" << (t_part.empty() ? "?" : t_part) << " count=" << count << std::hex << " args=" << a << ' ' << b << ' ' << c << ' '
          << d << ' ' << e << ' ' << f << ' ' << g << ' ' << h << ' ' << i;
    if (count && count <= 4096 && readable(entries, count * 8)) {
        const uint32_t idx = entries[(count - 1) * 2];
        const uint32_t* item = reinterpret_cast<uint32_t*>(0x93E878 + 0x44 * size_t(idx));
        g_log << " item#" << std::dec << idx << std::hex << ':';
        for (int k = 0; k < 17; ++k) g_log << ' ' << item[k];
        // Texture name (TextureInfo keeps it as a short string near the start) and the D3D objects reachable from the item.
        const char* tn = reinterpret_cast<const char*>(static_cast<uintptr_t>(item[9])) + 0xC;
        if (readable(tn, 24)) g_log << " TEX=" << std::string(tn, strnlen(tn, 24));
        // The mesh object (item[2] -> [0]): its dwords hold the buffers and the draw range; dumped to match with the D3D draws.
        const uint32_t* m1 = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(item[2]));
        if (readable(m1, 4)) {
            const uint32_t* y = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(m1[0]));
            if (readable(y, 0x80)) {
                g_log << " MESH:";
                for (int k = 0; k < 32; ++k) g_log << ' ' << y[k];
            }
        }
        // The per-slot strip object (item[0]): its dwords should hold this draw's index start and primitive count.
        const uint32_t* s0p = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(item[0]));
        if (readable(s0p, 0x80)) {
            g_log << " STRIP:";
            for (int k = 0; k < 32; ++k) g_log << ' ' << s0p[k];
        }
        g_log << " EFFECT=" << std::hex << item[4] << " TECH=" << std::dec << item[15];
    }
    g_log << std::dec << '\n';
}

HRESULT __stdcall dipHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIdx, UINT numVerts, UINT start, UINT prims) {
    if (g_capturing)
        describe(d, type, baseVertex, minIdx, numVerts, start, prims);
    if (g_highlight >= 0 && g_drawIndex == g_highlight) {
        if (!g_red) {
            ID3DBlob *code = nullptr, *err = nullptr;
            const char src[] = "float4 main():COLOR0{return float4(1,0,0,1);}";
            if (SUCCEEDED(D3DCompile(src, sizeof src - 1, nullptr, nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &err)) && code) {
                d->CreatePixelShader(static_cast<DWORD*>(code->GetBufferPointer()), &g_red);
                code->Release();
            }
            if (err) err->Release();
        }
        if (g_red) {
            IDirect3DPixelShader9* keep = nullptr;
            d->GetPixelShader(&keep);
            d->SetPixelShader(g_red);
            const HRESULT hr = g_origDip(d, type, baseVertex, minIdx, numVerts, start, prims);
            d->SetPixelShader(keep);
            if (keep) keep->Release();
            ++g_drawIndex;
            return hr;
        }
    }
    ++g_drawIndex;
    return g_origDip(d, type, baseVertex, minIdx, numVerts, start, prims);
}

bool pressed(int key) {
    static bool down[256]{};
    const bool now = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool edge = now && !down[key];
    down[key] = now;
    return edge;
}

HRESULT __stdcall presentHook(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    static int frames = 0;
    if (++frames % 600 == 1) {
        char line[160];
        snprintf(line, sizeof line, "present frames=%d lastDraws=%d highlight=%d\n", frames, g_drawIndex, g_highlight);
        std::ofstream(g_dir / "status.log", std::ios::app) << line;
    }
    if (g_capturing) { // the frame just drawn is complete: write it out
        std::ofstream(g_dir / "frame.log", std::ios::binary) << g_log.str();
        g_log.str({});
        g_capturing = false;
    }
    if (g_capture) {
        g_capture = false;
        g_capturing = true;
        g_log << "highlight=" << g_highlight << '\n';
    }
    // Keys: ; capture   ] next draw   [ previous draw   \ off   (F keys are taken by other mods)
    if (pressed('R') || pressed(VK_NUMPAD5)) g_capture = true;
    if (pressed('T') || pressed(VK_NUMPAD6)) { ++g_highlight; std::ofstream(g_dir / "highlight.txt") << g_highlight; }
    if ((pressed('Y') || pressed(VK_NUMPAD4)) && g_highlight >= 0) { --g_highlight; std::ofstream(g_dir / "highlight.txt") << g_highlight; }
    if (pressed('U') || pressed(VK_NUMPAD0)) { g_highlight = -1; std::ofstream(g_dir / "highlight.txt") << g_highlight; }
    g_drawIndex = 0;
    return g_origPresent(d, a, b, w, r);
}

void patch(void** table, int index, void* hook, void** orig) {
    DWORD old{};
    VirtualProtect(&table[index], sizeof(void*), PAGE_READWRITE, &old);
    *orig = table[index];
    table[index] = hook;
    VirtualProtect(&table[index], sizeof(void*), old, &old);
}

void installOn(IDirect3DDevice9* dev, void** table);

DWORD WINAPI worker(LPVOID) {
    IDirect3DDevice9* dev = nullptr;
    for (int i = 0; i < 600 && !dev; ++i) { // up to ~60 s for the game to create its device
        dev = *reinterpret_cast<IDirect3DDevice9**>(kDevicePtr);
        if (!dev) Sleep(100);
    }
    if (!dev) return 0;
    if (MH_Initialize() == MH_OK) { // the game's own functions, so a mesh can be tied to its car part
        MH_CreateHook(reinterpret_cast<void*>(0x7387E0), reinterpret_cast<void*>(&partHook), reinterpret_cast<void**>(&g_origPart));
        MH_CreateHook(reinterpret_cast<void*>(0x6E0AB0), reinterpret_cast<void*>(&submitHook), reinterpret_cast<void**>(&g_origSubmit));
        MH_EnableHook(MH_ALL_HOOKS);
    }
    // The game can recreate its device (resolution/mode change), so keep watching the pointer and hook each new vtable.
    void** hooked = nullptr;
    for (;;) {
        Sleep(1000);
        dev = *reinterpret_cast<IDirect3DDevice9**>(kDevicePtr);
        if (!dev) continue;
        void** table = *reinterpret_cast<void***>(dev);
        if (table == hooked) continue;
        hooked = table;
        installOn(dev, table);
    }
    return 0;
}

void installOn(IDirect3DDevice9* dev, void** table) {
    {
        HMODULE owner{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(table[17]), &owner);
        wchar_t name[MAX_PATH]{};
        if (owner) GetModuleFileNameW(owner, name, MAX_PATH);
        char line[600];
        snprintf(line, sizeof line, "device=%p vtable=%p present=%p dip=%p presentOwner=%ls\n", dev, table, table[17], table[82], name);
        std::ofstream(g_dir / "status.log", std::ios::app) << line;
    }
    // Hook the d3d9.dll function bodies (shared by every device), not the vtable slots: other mods rewrite the slots.
    static bool done = false;
    if (done) return;
    done = true;
    MH_CreateHook(table[82], reinterpret_cast<void*>(&dipHook), reinterpret_cast<void**>(&g_origDip));
    MH_CreateHook(table[17], reinterpret_cast<void*>(&presentHook), reinterpret_cast<void**>(&g_origPresent));
    MH_EnableHook(MH_ALL_HOOKS);
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        g_dir = std::filesystem::path(path).parent_path() / L"HQDecalsDiag";
        std::filesystem::create_directories(g_dir);
        CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    }
    return TRUE;
}
