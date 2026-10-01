/*
 * DBOPLEngine.cpp
 *
 * FmEngineApi ファサードの DBOPL (DOSBox OPL) バックエンド実装。
 * dbopl は OPL2 (YM3812) および OPL3 (YMF262) をエミュレートする。
 *
 * 対応チップ:
 *   "OPL"   — OPL1 相当 (dbopl は OPL2 コアで代替)
 *   "OPL2"  — YM3812  (2-op, mono)
 *   "OPL3"  — YMF262  (4-op, stereo) : reg 0x105 bit0 (NEW) で切り替え
 *
 * FmEngineApi の port 引数:
 *   OPL2 : port=0 のみ (0x388/0x389)
 *   OPL3 : port=0 → Primary (0x388/0x389)
 *          port=1 → Secondary (0x38A/0x38B)
 *
 * Generate():
 *   dbopl は NEW=0 のとき mono (GenerateBlock2 → 1 sample/frame)、
 *   NEW=1 のとき stereo (GenerateBlock3 → L,R interleaved 2 samples/frame) で
 *   int32 を出力する。ここでは float に変換し、FmEngineApi の out_l/out_r に書く。
 */

// FMENGINE_EXPORTS は CMakeLists.txt の target_compile_definitions で定義する
#include "../include/FmEngineApi.h"
#include "../../extern/dbopl/dbopl.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <new>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <atomic>

// ---------------------------------------------------------------------------
// 定数
// ---------------------------------------------------------------------------
// dbopl はクロックを受け取らず、内部レートを OPLRATE (14318180 / 288 Hz) に固定して
// テーブルを作る。これは次のクロックのチップに当たる。
static constexpr uint32_t DBOPL_OPL_CLOCK  = 3579545;   // OPL / OPL2 (clock / 72)
static constexpr uint32_t DBOPL_OPL3_CLOCK = 14318180;  // OPL3 (clock / 288)

// dbopl の出力は 16bit の範囲を想定した int32 (DOSBox ではそのままミキサーに渡す)。
// TL=0 の正弦波 1 チャンネルのピークが 4072 程度で、全チャンネルを重ねると
// 16bit の範囲を超えうる。
static constexpr float    DBOPL_SCALE = 1.0f / 32768.0f;

// ---------------------------------------------------------------------------
// 部位
// ---------------------------------------------------------------------------
static constexpr uint32_t kPartCount = FM_PART_OPL4_DO2 + 1;
static constexpr uint32_t kOpl3Parts =
    (1u << FM_PART_OPL3_AB) | (1u << FM_PART_OPL3_CD);
static_assert(kPartCount <= 32, "part mask is uint32_t");

struct PartGain {
    float l;
    float r;
};

static PartGain DefaultPartGain(uint32_t part) {
    const float g = (part == FM_PART_OPL3_CD) ? 0.0f : 1.0f;
    return { g, g };
}

// ---------------------------------------------------------------------------
// チップ種別
// ---------------------------------------------------------------------------
enum class ChipType { OPL, OPL2, OPL3 };

struct ChipEntry {
    ChipType        type;
    DBOPL::Handler  handler;
    // OPL3 の出力 C/D を作る2つ目の dbopl。dbopl は C0 の bit6/7 (出力 C/D) を
    // 扱わないので、C0 だけ bit6/7 を bit4/5 に付け替えて同じ書き込みを流し、
    // その A/B 出力を C/D として使う。dbopl は乱数を使わないので、同じ書き込みと
    // 同じ Generate を受けた2つは C0 の出力先ビット以外で同じ状態を保つ。
    std::unique_ptr<DBOPL::Handler> handler_cd;
    std::string     name;
    uint32_t        part_mask = 0;
    float           gain_l = 1.0f;
    float           gain_r = 1.0f;
    PartGain        part_gain[kPartCount];
};

// ---------------------------------------------------------------------------
// エンジン本体
// ---------------------------------------------------------------------------
struct FmEngineOpaque {
    uint32_t                 sample_rate;
    std::vector<ChipEntry>   chips;
    std::mutex               write_mutex;   // Write / SetGain 系 / Generate 間の排他

    explicit FmEngineOpaque(uint32_t sr) : sample_rate(sr) {}
};

// ---------------------------------------------------------------------------
// 対応チップ定義
// ---------------------------------------------------------------------------
struct SupportedChipDef {
    const char* name;
    ChipType    type;
    uint32_t    dbopl_clock;    // dbopl のテーブルが前提にしているクロック
    uint32_t    part_mask;
};

static const SupportedChipDef kSupportedChips[] = {
    { "OPL",  ChipType::OPL,  DBOPL_OPL_CLOCK,  0          },
    { "OPL2", ChipType::OPL2, DBOPL_OPL_CLOCK,  0          },
    { "OPL3", ChipType::OPL3, DBOPL_OPL3_CLOCK, kOpl3Parts },
};
static constexpr uint32_t kSupportedChipCount =
    (uint32_t)(sizeof(kSupportedChips) / sizeof(kSupportedChips[0]));

// ---------------------------------------------------------------------------
// ヘルパ: チップ定義を名前で検索
// ---------------------------------------------------------------------------
static const SupportedChipDef* FindChipDef(const char* name) {
    if (!name) return nullptr;
    for (uint32_t i = 0; i < kSupportedChipCount; ++i) {
        if (strcmp(kSupportedChips[i].name, name) == 0)
            return &kSupportedChips[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// ヘルパ: chip_id 検証
// ---------------------------------------------------------------------------
static bool ValidChipId(FmEngineHandle engine, uint32_t chip_id) {
    return engine && chip_id < engine->chips.size();
}

// clock のチップを sample_rate で鳴らすときに dbopl の Init に渡すレート。
// dbopl は OPLRATE と Init のレートの比で 1 サンプルあたりの時間を進めるので、
// Init のレートをクロックの比で縮めると、音程・エンベロープ・LFO がクロックに比例する。
// dbopl が受け取れない (0 か 32bit を超える) ときは 0。
static uint32_t DboplRate(uint32_t sample_rate, uint32_t clock, uint32_t dbopl_clock) {
    const uint64_t r = ((uint64_t)sample_rate * dbopl_clock + clock / 2) / clock;
    return (r == 0 || r > 0xFFFFFFFFu) ? 0 : (uint32_t)r;
}

static bool HasPart(const ChipEntry& entry, FmPart part) {
    const uint32_t p = (uint32_t)part;
    return p < kPartCount && (entry.part_mask & (1u << p)) != 0;
}

// ---------------------------------------------------------------------------
// ヘルパ: レジスタ書き込み (addr は dbopl の WriteAddr が返す 9bit アドレス)
// ---------------------------------------------------------------------------
// C0-C8 の bit4-7 は出力 A-D の許可。C/D 用の dbopl には C/D の許可を A/B の
// 位置に移して渡す。
static Bit8u RouteCdToAb(Bit8u c0) {
    return (Bit8u)((c0 & 0x0F) | ((c0 >> 2) & 0x30));
}

static void WriteChip(ChipEntry& entry, Bit32u addr, Bit8u value) {
    entry.handler.WriteReg(addr, value);
    if (entry.handler_cd) {
        const bool is_c0 = (addr & 0xF0) == 0xC0;
        entry.handler_cd->WriteReg(addr, is_c0 ? RouteCdToAb(value) : value);
    }
}

// ===========================================================================
//  API 実装
// ===========================================================================
extern "C" {

// ---------------------------------------------------------------------------
// FmEngine_Create / FmEngine_Destroy
// ---------------------------------------------------------------------------
FMENGINE_API FmEngineHandle FMENGINE_CALL
FmEngine_Create(uint32_t sample_rate) {
    if (sample_rate == 0) sample_rate = 44100;
    try {
        return new FmEngineOpaque(sample_rate);
    } catch (...) {
        return nullptr;
    }
}

FMENGINE_API void FMENGINE_CALL
FmEngine_Destroy(FmEngineHandle engine) {
    delete engine;
}

// ---------------------------------------------------------------------------
// FmEngine_Inquiry / FmEngine_GetSupportedChip
// ---------------------------------------------------------------------------
FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_Inquiry(FmEngineHandle /*engine*/) {
    return kSupportedChipCount;
}

FMENGINE_API const char* FMENGINE_CALL
FmEngine_GetSupportedChip(FmEngineHandle /*engine*/, uint32_t index) {
    if (index >= kSupportedChipCount) return nullptr;
    return kSupportedChips[index].name;
}

// ---------------------------------------------------------------------------
// FmEngine_AddChip
// ---------------------------------------------------------------------------
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_AddChip(FmEngineHandle engine, const char* name,
                 uint32_t clock, uint32_t* out_id) {
    if (!engine || !name) return FM_ERR_INVALID_ARG;

    const SupportedChipDef* def = FindChipDef(name);
    if (!def) return FM_ERR_UNKNOWN_CHIP;

    if (clock == 0) return FM_ERR_INVALID_ARG;
    const uint32_t dbopl_rate = DboplRate(engine->sample_rate, clock, def->dbopl_clock);
    if (dbopl_rate == 0) return FM_ERR_INVALID_ARG;

    try {
        engine->chips.emplace_back();
        ChipEntry& entry = engine->chips.back();
        entry.type = def->type;
        entry.name = def->name;
        entry.part_mask = def->part_mask;
        for (uint32_t p = 0; p < kPartCount; ++p)
            entry.part_gain[p] = DefaultPartGain(p);

        entry.handler.Init(dbopl_rate);

        // NEW (0x105) は立てない。実機のリセット直後と同じく OPL2 互換で始まる
        if (def->type == ChipType::OPL3) {
            entry.handler_cd = std::make_unique<DBOPL::Handler>();
            entry.handler_cd->Init(dbopl_rate);
        }

        if (out_id) *out_id = (uint32_t)(engine->chips.size() - 1);
        return FM_OK;
    } catch (...) {
        if (!engine->chips.empty()) engine->chips.pop_back();
        return FM_ERR_ALLOC;
    }
}

// ---------------------------------------------------------------------------
// FmEngine_GetChipName / FmEngine_GetNativeRate / FmEngine_GetSampleRate
// ---------------------------------------------------------------------------
FMENGINE_API const char* FMENGINE_CALL
FmEngine_GetChipName(FmEngineHandle engine, uint32_t chip_id) {
    if (!ValidChipId(engine, chip_id)) return nullptr;
    return engine->chips[chip_id].name.c_str();
}

FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetNativeRate(FmEngineHandle engine, uint32_t chip_id) {
    if (!ValidChipId(engine, chip_id)) return 0;
    // dbopl のネイティブ生成レート = ホストのサンプルレート（リサンプリングなし）
    return engine->sample_rate;
}

FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetSampleRate(FmEngineHandle engine) {
    if (!engine) return 0;
    return engine->sample_rate;
}

// ---------------------------------------------------------------------------
// FmEngine_Write
// ---------------------------------------------------------------------------
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_Write(FmEngineHandle engine, uint32_t chip_id,
               uint8_t reg, uint8_t value, uint32_t port) {
    if (!ValidChipId(engine, chip_id)) return FM_ERR_INVALID_ARG;

    ChipEntry& entry = engine->chips[chip_id];

    // port 引数の検証
    if (entry.type != ChipType::OPL3 && port != 0)
        return FM_ERR_INVALID_ARG;
    if (port > 1)
        return FM_ERR_INVALID_ARG;

    std::lock_guard<std::mutex> lock(engine->write_mutex);

    // dbopl の WriteAddr は DOSBox の I/O ポート番号の下位2ビットを受け取り、
    // 0 (0x388) を primary、2 (0x38A) を secondary のアドレスポートとして扱う。
    // NEW=0 の間、secondary への書き込みは 0x05 以外 primary に落ちる。
    Bit32u addr = entry.handler.WriteAddr(port * 2, reg);
    WriteChip(entry, addr, value);
    return FM_OK;
}

// ---------------------------------------------------------------------------
// FmEngine_SetGain / FmEngine_GetGain
// ---------------------------------------------------------------------------
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_SetGain(FmEngineHandle engine, uint32_t chip_id,
                 float gain_l, float gain_r) {
    if (!ValidChipId(engine, chip_id)) return FM_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(engine->write_mutex);
    engine->chips[chip_id].gain_l = gain_l;
    engine->chips[chip_id].gain_r = gain_r;
    return FM_OK;
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_GetGain(FmEngineHandle engine, uint32_t chip_id,
                 float* out_gain_l, float* out_gain_r) {
    if (!ValidChipId(engine, chip_id)) return FM_ERR_INVALID_ARG;
    if (!out_gain_l || !out_gain_r) return FM_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(engine->write_mutex);
    *out_gain_l = engine->chips[chip_id].gain_l;
    *out_gain_r = engine->chips[chip_id].gain_r;
    return FM_OK;
}

// ---------------------------------------------------------------------------
// FmEngine_SetPartGain / FmEngine_GetPartGain / FmEngine_GetPartMask
// ---------------------------------------------------------------------------
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_SetPartGain(FmEngineHandle engine, uint32_t chip_id, FmPart part,
                     float gain_l, float gain_r) {
    if (!ValidChipId(engine, chip_id)) return FM_ERR_INVALID_ARG;
    ChipEntry& entry = engine->chips[chip_id];
    if (!HasPart(entry, part)) return FM_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(engine->write_mutex);
    entry.part_gain[part] = { gain_l, gain_r };
    return FM_OK;
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_GetPartGain(FmEngineHandle engine, uint32_t chip_id, FmPart part,
                     float* out_gain_l, float* out_gain_r) {
    if (!ValidChipId(engine, chip_id)) return FM_ERR_INVALID_ARG;
    if (!out_gain_l || !out_gain_r) return FM_ERR_INVALID_ARG;
    const ChipEntry& entry = engine->chips[chip_id];
    if (!HasPart(entry, part)) return FM_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(engine->write_mutex);
    *out_gain_l = entry.part_gain[part].l;
    *out_gain_r = entry.part_gain[part].r;
    return FM_OK;
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_GetPartMask(FmEngineHandle engine, uint32_t chip_id, uint32_t* out_mask) {
    if (!ValidChipId(engine, chip_id)) return FM_ERR_INVALID_ARG;
    if (!out_mask) return FM_ERR_INVALID_ARG;
    *out_mask = engine->chips[chip_id].part_mask;
    return FM_OK;
}

// ---------------------------------------------------------------------------
// FmEngine_SetMemory / FmEngine_GetMemorySize
// dbopl は ADPCM/PCM メモリを持たない (OPL2/OPL3 はPCMなし)
// ---------------------------------------------------------------------------
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_SetMemory(FmEngineHandle engine, uint32_t chip_id,
                   FmMemoryType /*mem_type*/,
                   const uint8_t* /*data*/, uint32_t /*size*/) {
    if (!ValidChipId(engine, chip_id)) return FM_ERR_INVALID_ARG;
    return FM_ERR_UNAVAILABLE;   // OPL2/OPL3 は外部メモリ不要
}

FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetMemorySize(FmEngineHandle engine, uint32_t chip_id,
                       FmMemoryType /*mem_type*/) {
    if (!ValidChipId(engine, chip_id)) return 0;
    return 0;
}

// ---------------------------------------------------------------------------
// FmEngine_Generate
//
// dbopl 出力形式:
//   NEW=0 (opl3Active == 0): GenerateBlock2 → mono int32 配列 [samples]
//   NEW=1 (opl3Active != 0): GenerateBlock3 → stereo interleaved int32 [L0,R0,L1,R1,...]
//
// ここでは全チップをミックスして out_l / out_r に加算する。
// ---------------------------------------------------------------------------
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_Generate(FmEngineHandle engine,
                  float* out_l, float* out_r, uint32_t samples) {
    if (!engine || !out_l || !out_r || samples == 0)
        return FM_ERR_INVALID_ARG;

    memset(out_l, 0, samples * sizeof(float));
    memset(out_r, 0, samples * sizeof(float));
    if (engine->chips.empty())
        return FM_OK;

    // dbopl 作業バッファ: 出力 A/B 用と C/D 用に stereo 2 本ずつ
    std::vector<Bit32s> work(samples * 4, 0);
    Bit32s* ab = work.data();
    Bit32s* cd = ab + samples * 2;

    std::lock_guard<std::mutex> lock(engine->write_mutex);

    for (auto& entry : engine->chips) {
        entry.handler.Generate(ab, samples);
        // C/D 用の dbopl は、出力を使わないときも Generate して状態を揃えておく
        if (entry.handler_cd)
            entry.handler_cd->Generate(cd, samples);

        const bool is_opl3 = (entry.handler.chip.opl3Active != 0);

        float al = DBOPL_SCALE * entry.gain_l;
        float ar = DBOPL_SCALE * entry.gain_r;
        float cl = 0.0f;
        float cr = 0.0f;
        if (entry.part_mask & (1u << FM_PART_OPL3_AB)) {
            al *= entry.part_gain[FM_PART_OPL3_AB].l;
            ar *= entry.part_gain[FM_PART_OPL3_AB].r;
        }
        // ymfm (YMEngine) と同じく、NEW=0 の間は C/D に何も出さない
        if (is_opl3 && entry.handler_cd) {
            cl = DBOPL_SCALE * entry.gain_l * entry.part_gain[FM_PART_OPL3_CD].l;
            cr = DBOPL_SCALE * entry.gain_r * entry.part_gain[FM_PART_OPL3_CD].r;
        }

        if (is_opl3) {
            for (uint32_t i = 0; i < samples; ++i) {
                out_l[i] += (float)ab[i * 2]     * al + (float)cd[i * 2]     * cl;
                out_r[i] += (float)ab[i * 2 + 1] * ar + (float)cd[i * 2 + 1] * cr;
            }
        } else {
            for (uint32_t i = 0; i < samples; ++i) {
                out_l[i] += (float)ab[i] * al;
                out_r[i] += (float)ab[i] * ar;
            }
        }
    }

    // 仕様の出力範囲 [-1.0, 1.0] に収める。範囲内の信号は変えない
    for (uint32_t i = 0; i < samples; ++i) {
        out_l[i] = std::clamp(out_l[i], -1.0f, 1.0f);
        out_r[i] = std::clamp(out_r[i], -1.0f, 1.0f);
    }

    return FM_OK;
}

} // extern "C"
