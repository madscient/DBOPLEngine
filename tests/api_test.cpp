// api_test.cpp
// DBOPLEngine の FmEngineApi を DLL 越しに叩く試験。
//   api_test <DBOPLEngine の DLL / .so のパス>
// 失敗した項目があれば終了コードが 0 以外になる。
//
// 出力 C/D は、同じ音を A/B に出したときの出力と突き合わせて確かめる。
// C/D は A/B と同じ合成を別の dbopl で走らせて作っているので、一致は厳密に見る。

#include "FmEngineApi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace {

// ---------------------------------------------------------
//  DLL の読み込み
// ---------------------------------------------------------
struct Api {
    FmEngineHandle (FMENGINE_CALL *Create)(uint32_t);
    void        (FMENGINE_CALL *Destroy)(FmEngineHandle);
    uint32_t    (FMENGINE_CALL *Inquiry)(FmEngineHandle);
    const char* (FMENGINE_CALL *GetSupportedChip)(FmEngineHandle, uint32_t);
    FmResult    (FMENGINE_CALL *AddChip)(FmEngineHandle, const char*, uint32_t, uint32_t*);
    const char* (FMENGINE_CALL *GetChipName)(FmEngineHandle, uint32_t);
    uint32_t    (FMENGINE_CALL *GetNativeRate)(FmEngineHandle, uint32_t);
    uint32_t    (FMENGINE_CALL *GetSampleRate)(FmEngineHandle);
    FmResult    (FMENGINE_CALL *Write)(FmEngineHandle, uint32_t, uint8_t, uint8_t, uint32_t);
    FmResult    (FMENGINE_CALL *SetGain)(FmEngineHandle, uint32_t, float, float);
    FmResult    (FMENGINE_CALL *GetGain)(FmEngineHandle, uint32_t, float*, float*);
    FmResult    (FMENGINE_CALL *SetPartGain)(FmEngineHandle, uint32_t, FmPart, float, float);
    FmResult    (FMENGINE_CALL *GetPartGain)(FmEngineHandle, uint32_t, FmPart, float*, float*);
    FmResult    (FMENGINE_CALL *GetPartMask)(FmEngineHandle, uint32_t, uint32_t*);
    FmResult    (FMENGINE_CALL *SetMemory)(FmEngineHandle, uint32_t, FmMemoryType, const uint8_t*, uint32_t);
    FmResult    (FMENGINE_CALL *SetMemoryEx)(FmEngineHandle, uint32_t, FmMemoryType, uint32_t,
                                             uint8_t*, uint32_t, FmMemoryAccess);
    uint32_t    (FMENGINE_CALL *GetMemorySize)(FmEngineHandle, uint32_t, FmMemoryType);
    FmResult    (FMENGINE_CALL *Generate)(FmEngineHandle, float*, float*, uint32_t);
};

void* openLibrary(const char* path) {
#ifdef _WIN32
    return (void*)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW);
#endif
}

void* findSymbol(void* lib, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

// 必須シンボルが欠けていれば false。任意シンボルは欠けていれば nullptr のまま
bool loadApi(const char* path, Api& api) {
    void* lib = openLibrary(path);
    if (!lib) {
        std::printf("cannot load %s\n", path);
        return false;
    }
    bool ok = true;
    auto bind = [&](auto& fn, const char* name, bool required) {
        void* p = findSymbol(lib, name);
        if (!p && required) { std::printf("missing export: %s\n", name); ok = false; }
        std::memcpy(&fn, &p, sizeof(p));
    };
    bind(api.Create,           "FmEngine_Create",           true);
    bind(api.Destroy,          "FmEngine_Destroy",          true);
    bind(api.Inquiry,          "FmEngine_Inquiry",          true);
    bind(api.GetSupportedChip, "FmEngine_GetSupportedChip", true);
    bind(api.AddChip,          "FmEngine_AddChip",          true);
    bind(api.GetChipName,      "FmEngine_GetChipName",      true);
    bind(api.GetNativeRate,    "FmEngine_GetNativeRate",    true);
    bind(api.GetSampleRate,    "FmEngine_GetSampleRate",    true);
    bind(api.Write,            "FmEngine_Write",            true);
    bind(api.SetGain,          "FmEngine_SetGain",          true);
    bind(api.GetGain,          "FmEngine_GetGain",          true);
    bind(api.SetPartGain,      "FmEngine_SetPartGain",      false);
    bind(api.GetPartGain,      "FmEngine_GetPartGain",      false);
    bind(api.GetPartMask,      "FmEngine_GetPartMask",      false);
    bind(api.SetMemory,        "FmEngine_SetMemory",        true);
    bind(api.SetMemoryEx,      "FmEngine_SetMemoryEx",      false);
    bind(api.GetMemorySize,    "FmEngine_GetMemorySize",    true);
    bind(api.Generate,         "FmEngine_Generate",         true);
    return ok;
}

Api A;
int g_fails = 0;

void check(const std::string& what, bool ok) {
    std::printf("%-72s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) ++g_fails;
}

bool hasPartApi() {
    return A.SetPartGain && A.GetPartGain && A.GetPartMask;
}

// ---------------------------------------------------------
//  1 チップだけのエンジンを作り、書き込んで鳴らす
// ---------------------------------------------------------
constexpr uint32_t kRate = 44100;
constexpr uint32_t kFrames = 4410;  // 0.1 秒

// 試験のレジスタ値 (F-Number など) が前提にしているクロック
constexpr uint32_t kClockOpl  = 3579545;
constexpr uint32_t kClockOpl3 = 14318180;

uint32_t clockFor(const char* name) {
    return std::strcmp(name, "OPL3") == 0 ? kClockOpl3 : kClockOpl;
}

struct Reg { uint8_t reg, val; };
using Regs = std::vector<Reg>;

struct Out {
    std::vector<float> l, r;
};

struct Engine {
    FmEngineHandle h = nullptr;
    explicit Engine(uint32_t rate = kRate) : h(A.Create(rate)) {}
    ~Engine() { A.Destroy(h); }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    uint32_t add(const char* name) { return add(name, clockFor(name)); }
    uint32_t add(const char* name, uint32_t clock) {
        uint32_t id = 0xFFFFFFFFu;
        if (A.AddChip(h, name, clock, &id) != FM_OK) {
            std::printf("AddChip(%s) failed\n", name);
            ++g_fails;
        }
        return id;
    }
    void write(uint32_t id, const Regs& regs, uint32_t port = 0) {
        for (const auto& r : regs) A.Write(h, id, r.reg, r.val, port);
    }
    Out render() {
        Out o;
        o.l.resize(kFrames);
        o.r.resize(kFrames);
        A.Generate(h, o.l.data(), o.r.data(), kFrames);
        return o;
    }
};

// setup はチップを足したあとの書き込みやゲイン設定を行う
Out play(const char* chip, const std::function<void(Engine&, uint32_t)>& setup) {
    Engine e;
    uint32_t id = e.add(chip);
    setup(e, id);
    return e.render();
}

double rms(const std::vector<float>& v) {
    double acc = 0.0;
    for (float s : v) acc += (double)s * s;
    return std::sqrt(acc / (double)v.size());
}

double peak(const std::vector<float>& v) {
    double m = 0.0;
    for (float s : v) m = std::fmax(m, std::fabs((double)s));
    return m;
}

// TL=0 の正弦波 1 チャンネルの RMS は 0.06 程度
constexpr double kAudible = 1e-3;

bool audible(const std::vector<float>& v) { return rms(v) > kAudible; }
bool zero(const std::vector<float>& v) {
    for (float s : v) if (s != 0.0f) return false;
    return true;
}
bool zero(const Out& o) { return zero(o.l) && zero(o.r); }
bool same(const Out& a, const Out& b) { return a.l == b.l && a.r == b.r; }

// a が b を kl / kr 倍したものに一致するか (float の丸めの分だけ許す)
bool scaled(const Out& a, const Out& b, float kl, float kr) {
    if (a.l.size() != b.l.size()) return false;
    double peak = 0.0, diff = 0.0;
    for (size_t i = 0; i < a.l.size(); ++i) {
        peak = std::fmax(peak, std::fabs((double)b.l[i]) + std::fabs((double)b.r[i]));
        diff = std::fmax(diff, std::fabs((double)a.l[i] - (double)b.l[i] * kl));
        diff = std::fmax(diff, std::fabs((double)a.r[i] - (double)b.r[i] * kr));
    }
    return peak > 0.0 && diff <= peak * 1e-6;
}

// ---------------------------------------------------------
//  レジスタ列
// ---------------------------------------------------------
// ch (0-8) のモジュレータのオペレータ番号。キャリアは +3
constexpr uint8_t kModOp[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12 };

// ch に正弦波の 2-op FM 音を出す。c0 は C0 レジスタ (bit4-7: 出力 A-D の許可)
Regs note(uint8_t ch, uint8_t c0) {
    const uint8_t m = kModOp[ch];
    const uint8_t c = (uint8_t)(m + 3);
    return {
        { (uint8_t)(0x20 + m), 0x21 }, { (uint8_t)(0x20 + c), 0x21 },   // EG-TYP=1 (持続), MULT=1
        { (uint8_t)(0x40 + m), 0x20 }, { (uint8_t)(0x40 + c), 0x00 },
        { (uint8_t)(0x60 + m), 0xF0 }, { (uint8_t)(0x60 + c), 0xF0 },
        { (uint8_t)(0x80 + m), 0x0F }, { (uint8_t)(0x80 + c), 0x0F },
        { (uint8_t)(0xE0 + m), 0x00 }, { (uint8_t)(0xE0 + c), 0x00 },
        { (uint8_t)(0xA0 + ch), 0x44 },
        { (uint8_t)(0xC0 + ch), c0 },
        { (uint8_t)(0xB0 + ch), 0x32 },   // KEY ON, BLOCK 4
    };
}

// ch0 と ch3 を組にした 4-op の FM-FM 音。c0 は両チャンネルの C0
Regs fourOpNote(uint8_t c0) {
    Regs regs;
    const uint8_t ops[4] = { 0x00, 0x03, 0x08, 0x0B };
    for (uint8_t op : ops) {
        regs.push_back({ (uint8_t)(0x20 + op), 0x21 });
        regs.push_back({ (uint8_t)(0x40 + op), (uint8_t)(op == 0x0B ? 0x00 : 0x20) });
        regs.push_back({ (uint8_t)(0x60 + op), 0xF0 });
        regs.push_back({ (uint8_t)(0x80 + op), 0x0F });
        regs.push_back({ (uint8_t)(0xE0 + op), 0x00 });
    }
    regs.push_back({ 0xA0, 0x44 });
    regs.push_back({ 0xC0, c0 });
    regs.push_back({ 0xC3, c0 });
    regs.push_back({ 0xB0, 0x32 });
    return regs;
}

constexpr uint8_t kOutA = 0x10, kOutB = 0x20, kOutC = 0x40, kOutD = 0x80;

// OPL3 モード (NEW=1) にする。AddChip の直後は NEW=0
void enableOpl3(Engine& e, uint32_t id) {
    A.Write(e.h, id, 0x05, 0x01, 1);
}

// NEW=1 の OPL3 の ch0 に c0 の出力先で音を出し、gains で部位のゲインを設定して鳴らす
Out opl3Note(uint8_t c0, const std::function<void(Engine&, uint32_t)>& gains = {}) {
    return play("OPL3", [&](Engine& e, uint32_t id) {
        enableOpl3(e, id);
        if (gains) gains(e, id);
        e.write(id, note(0, c0));
    });
}

// ---------------------------------------------------------
//  試験
// ---------------------------------------------------------
void testExports() {
    check("exports the part gain API (SetPartGain / GetPartGain / GetPartMask)",
          hasPartApi());
    check("does not export FmEngine_SetMemoryEx (no chip has external memory)",
          A.SetMemoryEx == nullptr);
}

void testChipList() {
    Engine e;
    const char* expected[] = { "OPL", "OPL2", "OPL3" };
    bool ok = A.Inquiry(e.h) == 3;
    for (uint32_t i = 0; ok && i < 3; ++i) {
        const char* n = A.GetSupportedChip(e.h, i);
        ok = n && std::strcmp(n, expected[i]) == 0;
    }
    check("supported chips are OPL, OPL2, OPL3", ok && A.GetSupportedChip(e.h, 3) == nullptr);
    uint32_t id = 0;
    check("AddChip rejects an unknown name",
          A.AddChip(e.h, "OPNA", kClockOpl, &id) == FM_ERR_UNKNOWN_CHIP);
}

// 正の向きのゼロ交差の間隔から周波数を求める (交差の位置は線形補間)
double frequency(const std::vector<float>& v) {
    double first = 0.0, last = 0.0;
    int n = 0;
    for (size_t i = 1; i < v.size(); ++i) {
        if (v[i - 1] < 0.0f && v[i] >= 0.0f) {
            const double t = (double)(i - 1) + v[i - 1] / (double)(v[i - 1] - v[i]);
            if (n == 0) first = t;
            last = t;
            ++n;
        }
    }
    return n >= 2 ? (n - 1) * (double)kRate / (last - first) : 0.0;
}

void testClock() {
    Engine e;
    uint32_t id = 0;
    bool rejected = true;
    for (const char* n : { "OPL", "OPL2", "OPL3" })
        rejected = rejected && A.AddChip(e.h, n, 0, &id) == FM_ERR_INVALID_ARG;
    check("AddChip rejects clock 0", rejected && A.GetChipName(e.h, 0) == nullptr);
    // 1 Hz を 44100 Hz で鳴らすには dbopl に 2^32 を超えるレートを渡すことになる
    check("AddChip rejects a clock dbopl cannot represent at the sample rate",
          A.AddChip(e.h, "OPL2", 1, &id) == FM_ERR_INVALID_ARG && A.GetChipName(e.h, 0) == nullptr);

    for (const char* n : { "OPL2", "OPL3" }) {
        const uint32_t base = clockFor(n);
        auto pitch = [&](uint32_t clock) {
            Engine x;
            uint32_t cid = x.add(n, clock);
            Regs regs = note(0, kOutA | kOutB);
            for (auto& r : regs)
                if (r.reg == 0x40) r.val = 0x3F;   // モジュレータを消して正弦波にする
            x.write(cid, regs);
            return frequency(x.render().l);
        };
        const double f = pitch(base);
        check(std::string(n) + ": F-Number 0x244 BLOCK 4 is 440 Hz at clock " + std::to_string(base),
              std::fabs(f - 440.0) < 0.5);
        check(std::string(n) + ": the pitch follows the clock (x2, x0.5)",
              std::fabs(pitch(base * 2) / f - 2.0) < 0.002 &&
              std::fabs(pitch(base / 2) / f - 0.5) < 0.002);

        // クロックを 2 倍にしたチップは、時間を半分に縮めたように鳴る。
        // ビブラート・トレモロ (LFO) と減衰 (エンベロープ) を入れて、音程以外も見る
        auto lfoDecay = [&](uint32_t rate, uint32_t clock) {
            Engine x(rate);
            uint32_t cid = x.add(n, clock);
            Regs regs = note(0, kOutA | kOutB);
            for (auto& r : regs) {
                if (r.reg == 0x20 || r.reg == 0x23) r.val = 0xC1;   // AM, VIB, EG-TYP=0, MULT=1
                if (r.reg == 0x60 || r.reg == 0x63) r.val = 0xF5;   // AR=15, DR=5
                if (r.reg == 0x80 || r.reg == 0x83) r.val = 0x45;   // SL=4, RR=5
            }
            regs.push_back({ 0xBD, 0xC0 });                         // AM・VIB の深さ最大
            x.write(cid, regs);
            return x.render();
        };
        const Out fast = lfoDecay(kRate, base * 2);
        check(std::string(n) + ": x2 clock at 44100 Hz equals x1 clock at 22050 Hz (LFO, envelope)",
              audible(fast.l) && same(fast, lfoDecay(kRate / 2, base)));
    }
}

void testPort() {
    Engine e;
    uint32_t opl2 = e.add("OPL2");
    uint32_t opl3 = e.add("OPL3");
    check("Write rejects port 1 on OPL2",
          A.Write(e.h, opl2, 0x20, 0x01, 1) == FM_ERR_INVALID_ARG);
    check("Write rejects port 2 on OPL3",
          A.Write(e.h, opl3, 0x20, 0x01, 2) == FM_ERR_INVALID_ARG);
}

void testMemory() {
    Engine e;
    uint32_t opl3 = e.add("OPL3");
    uint8_t data[16] = {};
    check("SetMemory is unavailable",
          A.SetMemory(e.h, opl3, FM_MEM_PCM, data, sizeof(data)) == FM_ERR_UNAVAILABLE &&
          A.GetMemorySize(e.h, opl3, FM_MEM_PCM) == 0);
}

void testOpl2() {
    Out o = play("OPL2", [](Engine& e, uint32_t id) { e.write(id, note(0, 0x00)); });
    check("OPL2: a note is audible and identical on L and R", audible(o.l) && o.l == o.r);
}

void testLevel() {
    Out one = play("OPL2", [](Engine& e, uint32_t id) { e.write(id, note(0, 0x00)); });
    check("a TL=0 sine on one channel peaks at about 0.12 (4072 / 32768)",
          peak(one.l) > 0.10 && peak(one.l) < 0.15);

    // 18 チャンネルを同じ位相で鳴らすと 1.0 を超える
    Out all = play("OPL3", [](Engine& e, uint32_t id) {
        enableOpl3(e, id);
        for (uint32_t port = 0; port < 2; ++port)
            for (uint8_t ch = 0; ch < 9; ++ch)
                e.write(id, note(ch, kOutA | kOutB), port);
    });
    check("the mix is clipped to [-1.0, 1.0]",
          peak(all.l) == 1.0 && peak(all.r) == 1.0);
}

void testOpl3Output() {
    Out boot = play("OPL3", [](Engine& e, uint32_t id) { e.write(id, note(0, 0x00)); });
    check("OPL3 starts with NEW=0: a note without output bits is identical on L and R",
          audible(boot.l) && boot.l == boot.r);
    Out fall = play("OPL3", [](Engine& e, uint32_t id) { e.write(id, note(0, 0x00), 1); });
    check("OPL3 with NEW=0: port 1 writes land on the primary bank",
          audible(fall.l) && fall.l == fall.r);

    // primary の ch0 を A に、secondary の ch0 (ch9) を B に出す
    Out sec = play("OPL3", [](Engine& e, uint32_t id) {
        enableOpl3(e, id);
        e.write(id, note(0, kOutA), 0);
        e.write(id, note(0, kOutB), 1);
    });
    check("OPL3: port 1 addresses the secondary bank apart from the primary",
          audible(sec.l) && audible(sec.r));

    Out a = opl3Note(kOutA);
    Out b = opl3Note(kOutB);
    check("OPL3: output A goes to L only", audible(a.l) && zero(a.r));
    check("OPL3: output B goes to R only", zero(b.l) && audible(b.r));
    check("OPL3: a channel with no output bit is silent", zero(opl3Note(0x00)));
}

void testPartApi() {
    if (!hasPartApi()) {
        check("part gain API tests (skipped: symbols are missing)", false);
        return;
    }
    Engine e;
    uint32_t opl  = e.add("OPL");
    uint32_t opl2 = e.add("OPL2");
    uint32_t opl3 = e.add("OPL3");
    const uint32_t opl3Parts = (1u << FM_PART_OPL3_AB) | (1u << FM_PART_OPL3_CD);

    uint32_t m1 = 1, m2 = 1, m3 = 0;
    check("GetPartMask: OPL and OPL2 have no part, OPL3 has AB and CD",
          A.GetPartMask(e.h, opl, &m1) == FM_OK && m1 == 0 &&
          A.GetPartMask(e.h, opl2, &m2) == FM_OK && m2 == 0 &&
          A.GetPartMask(e.h, opl3, &m3) == FM_OK && m3 == opl3Parts);
    check("GetPartMask rejects an unknown chip_id and a null pointer",
          A.GetPartMask(e.h, 1000, &m1) == FM_ERR_INVALID_ARG &&
          A.GetPartMask(e.h, opl3, nullptr) == FM_ERR_INVALID_ARG);

    float l = -1.0f, r = -1.0f;
    bool ab = A.GetPartGain(e.h, opl3, FM_PART_OPL3_AB, &l, &r) == FM_OK && l == 1.0f && r == 1.0f;
    bool cd = A.GetPartGain(e.h, opl3, FM_PART_OPL3_CD, &l, &r) == FM_OK && l == 0.0f && r == 0.0f;
    check("OPL3 part gains default to AB = 1.0, CD = 0", ab && cd);

    check("SetPartGain / GetPartGain reject a part the chip does not have",
          A.SetPartGain(e.h, opl2, FM_PART_OPL3_AB, 0.5f, 0.5f) == FM_ERR_INVALID_ARG &&
          A.SetPartGain(e.h, opl3, FM_PART_OPN_FM, 0.5f, 0.5f) == FM_ERR_INVALID_ARG &&
          A.SetPartGain(e.h, opl3, (FmPart)40, 0.5f, 0.5f) == FM_ERR_INVALID_ARG &&
          A.SetPartGain(e.h, opl3, (FmPart)-1, 0.5f, 0.5f) == FM_ERR_INVALID_ARG &&
          A.GetPartGain(e.h, opl, FM_PART_OPL3_CD, &l, &r) == FM_ERR_INVALID_ARG &&
          A.SetPartGain(e.h, 1000, FM_PART_OPL3_AB, 0.5f, 0.5f) == FM_ERR_INVALID_ARG);
    check("SetPartGain then GetPartGain returns the values",
          A.SetPartGain(e.h, opl3, FM_PART_OPL3_CD, 0.25f, 0.75f) == FM_OK &&
          A.GetPartGain(e.h, opl3, FM_PART_OPL3_CD, &l, &r) == FM_OK && l == 0.25f && r == 0.75f);
}

void testOpl3Parts() {
    if (!hasPartApi()) {
        check("OPL3 part output tests (skipped: symbols are missing)", false);
        return;
    }
    auto partGain = [](FmPart part, float l, float r) {
        return [=](Engine& e, uint32_t id) { A.SetPartGain(e.h, id, part, l, r); };
    };
    const Out refAB = opl3Note(kOutA | kOutB);
    check("OPL3: a note on A/B is audible on L and R", audible(refAB.l) && audible(refAB.r));

    check("OPL3: C/D are muted by default",
          zero(opl3Note(kOutC | kOutD)));
    check("OPL3: C/D with CD gain 1.0 equal the same note on A/B",
          same(opl3Note(kOutC | kOutD, partGain(FM_PART_OPL3_CD, 1.0f, 1.0f)), refAB));
    Out c = opl3Note(kOutC, partGain(FM_PART_OPL3_CD, 1.0f, 1.0f));
    Out d = opl3Note(kOutD, partGain(FM_PART_OPL3_CD, 1.0f, 1.0f));
    check("OPL3: output C goes to L only, D to R only",
          audible(c.l) && zero(c.r) && zero(d.l) && audible(d.r));
    check("OPL3: A-D all on with AB = CD = 1.0 doubles the A/B note",
          scaled(opl3Note(kOutA | kOutB | kOutC | kOutD, partGain(FM_PART_OPL3_CD, 1.0f, 1.0f)),
                 refAB, 2.0f, 2.0f));
    check("OPL3: AB gain does not reach C/D",
          same(opl3Note(kOutC | kOutD, [](Engine& e, uint32_t id) {
                   A.SetPartGain(e.h, id, FM_PART_OPL3_AB, 0.0f, 0.0f);
                   A.SetPartGain(e.h, id, FM_PART_OPL3_CD, 1.0f, 1.0f);
               }), refAB));

    check("OPL3: chip gain x AB gain applies to A/B",
          scaled(opl3Note(kOutA | kOutB, [](Engine& e, uint32_t id) {
                     A.SetGain(e.h, id, 0.5f, 0.25f);
                     A.SetPartGain(e.h, id, FM_PART_OPL3_AB, 0.5f, 2.0f);
                 }), refAB, 0.25f, 0.5f));
    check("OPL3: chip gain x CD gain applies to C/D",
          scaled(opl3Note(kOutC | kOutD, [](Engine& e, uint32_t id) {
                     A.SetGain(e.h, id, 0.5f, 0.25f);
                     A.SetPartGain(e.h, id, FM_PART_OPL3_CD, 0.5f, 2.0f);
                 }), refAB, 0.25f, 0.5f));

    {
        Engine ref, e;
        uint32_t rid = ref.add("OPL3");
        uint32_t id = e.add("OPL3");
        enableOpl3(ref, rid);
        enableOpl3(e, id);
        ref.write(rid, note(0, kOutA | kOutB));
        e.write(id, note(0, kOutC | kOutD));
        ref.render();
        Out muted = e.render();
        A.SetPartGain(e.h, id, FM_PART_OPL3_CD, 1.0f, 1.0f);
        check("OPL3: C/D stay in step with A/B while CD gain is 0 and then raised",
              zero(muted) && same(e.render(), ref.render()));
    }

    // 標準でないクロックでも、C/D 用の dbopl は A/B 用と同じ換算で動く
    auto atClock = [](uint8_t c0, float cd) {
        Engine x;
        uint32_t cid = x.add("OPL3", 15000000);
        enableOpl3(x, cid);
        A.SetPartGain(x.h, cid, FM_PART_OPL3_CD, cd, cd);
        x.write(cid, note(0, c0));
        return x.render();
    };
    const Out abAtClock = atClock(kOutA | kOutB, 0.0f);
    check("OPL3 at 15 MHz: C/D with CD gain 1.0 equal the same note on A/B",
          audible(abAtClock.l) && abAtClock.l != refAB.l &&
          same(atClock(kOutC | kOutD, 1.0f), abAtClock));

    auto fourOp = [&](uint8_t c0, const std::function<void(Engine&, uint32_t)>& gains) {
        return play("OPL3", [&](Engine& e, uint32_t id) {
            enableOpl3(e, id);
            A.Write(e.h, id, 0x04, 0x01, 1);   // ch0 と ch3 を 4-op にする
            if (gains) gains(e, id);
            e.write(id, fourOpNote(c0));
        });
    };
    const Out ref4 = fourOp(kOutA | kOutB, {});
    check("OPL3: a 4-op note on C/D with CD gain 1.0 equals it on A/B",
          audible(ref4.l) && ref4.l != refAB.l &&
          same(fourOp(kOutC | kOutD, partGain(FM_PART_OPL3_CD, 1.0f, 1.0f)), ref4));

    // dbopl はリズム音に C0 の出力先ビットを使わない
    auto bassDrum = [&](const std::function<void(Engine&, uint32_t)>& gains) {
        return play("OPL3", [&](Engine& e, uint32_t id) {
            enableOpl3(e, id);
            if (gains) gains(e, id);
            Regs regs = note(6, 0x00);
            regs.back().val &= (uint8_t)~0x20;   // ch6 は KEY ON せず、BD で鳴らす
            regs.push_back({ 0xBD, 0x30 });      // リズムモード + BD
            e.write(id, regs);
        });
    };
    const Out bd = bassDrum({});
    check("OPL3: rhythm ignores C0 routing and comes out of A/B",
          audible(bd.l) && bd.l == bd.r);
    check("OPL3: rhythm also comes out of C/D when CD gain is non-zero",
          scaled(bassDrum(partGain(FM_PART_OPL3_CD, 1.0f, 1.0f)), bd, 2.0f, 2.0f));

    // NEW=0 の間は C0 の出力先ビットを見ず、A/B にだけ出す (ymfm と同じ)
    auto opl2Mode = [](Engine& e, uint32_t id) { A.Write(e.h, id, 0x05, 0x00, 1); };
    Out mono = play("OPL3", [&](Engine& e, uint32_t id) {
        opl2Mode(e, id);
        e.write(id, note(0, kOutC | kOutD));
    });
    check("OPL3 with NEW=0: a note is identical on L and R regardless of C0 routing",
          audible(mono.l) && mono.l == mono.r);
    Out monoCd = play("OPL3", [&](Engine& e, uint32_t id) {
        opl2Mode(e, id);
        A.SetPartGain(e.h, id, FM_PART_OPL3_AB, 0.0f, 0.0f);
        A.SetPartGain(e.h, id, FM_PART_OPL3_CD, 1.0f, 1.0f);
        e.write(id, note(0, kOutA | kOutB | kOutC | kOutD));
    });
    check("OPL3 with NEW=0: nothing comes out of C/D", zero(monoCd));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: api_test <path to DBOPLEngine library>\n");
        return 2;
    }
    if (!loadApi(argv[1], A)) return 1;

    testExports();
    testChipList();
    testClock();
    testPort();
    testMemory();
    testOpl2();
    testLevel();
    testOpl3Output();
    testPartApi();
    testOpl3Parts();

    std::printf("%s (%d failed)\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
