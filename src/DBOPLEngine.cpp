/*
 * DBOPLEngine.cpp
 *
 * FmEngineApi ファサードの DBOPL (DOSBox OPL) バックエンド実装。
 * dbopl は OPL2 (YM3812) および OPL3 (YMF262) をエミュレートする。
 *
 * 対応チップ:
 *   "OPL"   — OPL1 相当 (dbopl は OPL2 コアで代替)
 *   "OPL2"  — YM3812  (2-op, mono)
 *   "OPL3"  — YMF262  (4-op, stereo) : reg104 bit0 で切り替え
 *
 * FmEngineApi の port 引数:
 *   OPL2 : port=0 のみ (0x388/0x389)
 *   OPL3 : port=0 → Primary (0x388/0x389)
 *          port=1 → Secondary (0x38A/0x38B)
 *
 * Generate():
 *   dbopl は Bit32s (int32) インターリーブまたは非インターリーブで出力する。
 *   OPL2 は mono (GenerateBlock2 → 1 sample/frame)
 *   OPL3 は stereo (GenerateBlock3 → L,R interleaved 2 samples/frame)
 *   ここでは int32 を float に変換し、FmEngineApi の out_l/out_r に書く。
 */

// FMENGINE_EXPORTS は CMakeLists.txt の target_compile_definitions で定義する
#include "../include/FmEngineApi.h"
#include "../../extern/dbopl/dbopl.h"

#include <cstdlib>
#include <cstring>
#include <cmath>
#include <new>
#include <mutex>
#include <vector>
#include <string>
#include <atomic>

// ---------------------------------------------------------------------------
// 定数
// ---------------------------------------------------------------------------
static constexpr uint32_t DBOPL_OPL_CLOCK  = 3579545;   // OPL standard clock
static constexpr uint32_t DBOPL_OPL3_CLOCK = 14318180;  // OPL3 standard clock

// int32 sample の最大振幅 (dbopl の出力スケール)
// GenerateBlock2/3 は [-32768*256, 32767*256] 程度の値を返す
static constexpr float    DBOPL_SCALE = 1.0f / (32768.0f * 256.0f);

// ---------------------------------------------------------------------------
// チップ種別
// ---------------------------------------------------------------------------
enum class ChipType { OPL, OPL2, OPL3 };

struct ChipEntry {
    ChipType        type;
    DBOPL::Handler  handler;
    std::string     name;
    float           gain_l = 1.0f;
    float           gain_r = 1.0f;
    // OPL3 の reg104 の opl3 ビット管理は WriteReg 経由で自然に行う
};

// ---------------------------------------------------------------------------
// エンジン本体
// ---------------------------------------------------------------------------
struct FmEngineOpaque {
    uint32_t                 sample_rate;
    std::vector<ChipEntry>   chips;
    std::mutex               write_mutex;   // Write / Generate 間の排他

    explicit FmEngineOpaque(uint32_t sr) : sample_rate(sr) {}
};

// ---------------------------------------------------------------------------
// 対応チップ定義
// ---------------------------------------------------------------------------
struct SupportedChipDef {
    const char* name;
    ChipType    type;
    uint32_t    default_clock;
};

static const SupportedChipDef kSupportedChips[] = {
    { "OPL",  ChipType::OPL,  DBOPL_OPL_CLOCK  },
    { "OPL2", ChipType::OPL2, DBOPL_OPL_CLOCK  },
    { "OPL3", ChipType::OPL3, DBOPL_OPL3_CLOCK },
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

    uint32_t use_clock = (clock != 0) ? clock : def->default_clock;
    (void)use_clock; // dbopl は Init(sample_rate) のみ受け取るため現時点では未使用

    try {
        engine->chips.emplace_back();
        ChipEntry& entry = engine->chips.back();
        entry.type = def->type;
        entry.name = def->name;

        // dbopl を初期化（Init は rate を受け取る; clock は内部テーブルに焼かれている）
        // dbopl の Init() はサンプルレートで初期化する
        entry.handler.Init(engine->sample_rate);

        // OPL3 の場合は reg104 の bit0 を立てて stereo/4op を有効化
        // port=0, addr=0x105 (secondary page reg104 = 0x105 in addr space)
        if (def->type == ChipType::OPL3) {
            // WriteAddr(port=1, val=0x05) → addr=0x105
            Bit32u addr = entry.handler.WriteAddr(1, 0x05);
            entry.handler.WriteReg(addr, 0x01); // opl3 enable bit
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
// OPL2: port=0 のみ。WriteAddr(0, reg) で直接アドレス決定。
// OPL3: port=0 → primary (reg 0x000–0x0FF)
//        port=1 → secondary (reg 0x100–0x1FF) は WriteAddr(1, reg) で得る
//
// FmEngineApi の reg 引数は 8bit レジスタアドレス。
// dbopl の WriteAddr は port と reg を合成して 10bit アドレスを返す。
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

    Bit32u addr = entry.handler.WriteAddr(port, reg);
    entry.handler.WriteReg(addr, value);
    return FM_OK;
}

// ---------------------------------------------------------------------------
// FmEngine_SetGain / FmEngine_GetGain
// ---------------------------------------------------------------------------
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_SetGain(FmEngineHandle engine, uint32_t chip_id,
                 float gain_l, float gain_r) {
    if (!ValidChipId(engine, chip_id)) return FM_ERR_INVALID_ARG;
    engine->chips[chip_id].gain_l = gain_l;
    engine->chips[chip_id].gain_r = gain_r;
    return FM_OK;
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_GetGain(FmEngineHandle engine, uint32_t chip_id,
                 float* out_gain_l, float* out_gain_r) {
    if (!ValidChipId(engine, chip_id)) return FM_ERR_INVALID_ARG;
    if (!out_gain_l || !out_gain_r) return FM_ERR_INVALID_ARG;
    *out_gain_l = engine->chips[chip_id].gain_l;
    *out_gain_r = engine->chips[chip_id].gain_r;
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
//   OPL2 (opl3Active == 0): GenerateBlock2 → mono int32 配列 [samples]
//   OPL3 (opl3Active != 0): GenerateBlock3 → stereo interleaved int32 [L0,R0,L1,R1,...]
//
// ここでは全チップをミックスして out_l / out_r に加算する。
// ---------------------------------------------------------------------------
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_Generate(FmEngineHandle engine,
                  float* out_l, float* out_r, uint32_t samples) {
    if (!engine || !out_l || !out_r || samples == 0)
        return FM_ERR_INVALID_ARG;
    if (engine->chips.empty()) {
        memset(out_l, 0, samples * sizeof(float));
        memset(out_r, 0, samples * sizeof(float));
        return FM_OK;
    }

    // 出力バッファをゼロ初期化
    memset(out_l, 0, samples * sizeof(float));
    memset(out_r, 0, samples * sizeof(float));

    // dbopl 作業バッファ (最大サンプル数 × 2 チャンネル)
    std::vector<Bit32s> work(samples * 2, 0);

    std::lock_guard<std::mutex> lock(engine->write_mutex);

    for (auto& entry : engine->chips) {
        // バッファをゼロクリア
        memset(work.data(), 0, samples * 2 * sizeof(Bit32s));

        entry.handler.Generate(work.data(), samples);

        bool is_opl3 = (entry.handler.chip.opl3Active != 0);
        float gl = entry.gain_l;
        float gr = entry.gain_r;

        if (is_opl3) {
            // stereo interleaved: L=work[i*2], R=work[i*2+1]
            for (uint32_t i = 0; i < samples; ++i) {
                out_l[i] += (float)work[i * 2]     * DBOPL_SCALE * gl;
                out_r[i] += (float)work[i * 2 + 1] * DBOPL_SCALE * gr;
            }
        } else {
            // mono: work[i] → both L and R
            for (uint32_t i = 0; i < samples; ++i) {
                float s = (float)work[i] * DBOPL_SCALE;
                out_l[i] += s * gl;
                out_r[i] += s * gr;
            }
        }
    }

    return FM_OK;
}

} // extern "C"
