# DBOPLEngine

**FmEngineApi** に準拠した DBOPL (DOSBox OPL エミュレータ) バックエンドライブラリです。  
[rofl0r/dbopl](https://github.com/rofl0r/dbopl) を git submodule として使用し、OPL2 / OPL3 チップをエミュレートします。

---

## 対応チップ

| チップ名 | 実チップ        | 説明                              |
|----------|-----------------|-----------------------------------|
| `OPL`    | YM3526 相当     | OPL1 (dbopl の OPL2 コアで代替)   |
| `OPL2`   | YM3812          | OPL2, 2-op FM, モノラル           |
| `OPL3`   | YMF262          | OPL3, 4-op FM, ステレオ           |

---

## ファイル構成

```
DBOPLEngine/
├── .gitmodules              ← サブモジュール定義
├── CMakeLists.txt
├── README.md
├── include/
│   └── FmEngineApi.h        ← FmEngineApi ファサードヘッダ (各エンジン共通)
├── extern/
│   └── dbopl/               ← git submodule: https://github.com/rofl0r/dbopl
│       ├── dbopl.h
│       └── dbopl.cpp
├── src/
│   └── DBOPLEngine.cpp      ← FmEngineApi 実装
└── tests/
    └── api_test.cpp         ← API 試験 (DLL を実行時にロードして叩く)
```

---

## セットアップ

```bash
git clone --recurse-submodules https://github.com/madscient/DBOPLEngine
```

または clone 後にサブモジュールを初期化:

```bash
git clone https://github.com/madscient/DBOPLEngine
cd DBOPLEngine
git submodule update --init --recursive
```

---

## ビルド

### Windows (Visual Studio 2022)

```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Linux / macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

成果物:

| プラットフォーム | 共有ライブラリ                    |
|------------------|-----------------------------------|
| Windows          | `build/bin/Release/DBOPLEngine.dll`      |
| Linux            | `build/bin/libDBOPLEngine.so`     |
| macOS            | `build/bin/libDBOPLEngine.dylib`  |

サブモジュールが初期化されていない場合、cmake configure 時に以下のエラーで停止します:

```
CMake Error: extern/dbopl が見つかりません。
  git submodule update --init --recursive
```

---

## 試験

ビルドすると API 試験 `api_test` も作られます。DLL を実行時にロードし、エクスポートされた関数だけを叩きます。

```bash
ctest --test-dir build -C Release --output-on-failure
```

DLL を直接指定して走らせることもできます。

```bash
api_test <DBOPLEngine の DLL / .so のパス>
```

試験をビルドしない場合は `-DDBOPL_BUILD_TESTS=OFF` を指定します。

---

## FmEngineTest との組み合わせ

[FmEngineTest](https://github.com/madscient/FMEngineTest) の `-e` オプションで DBOPLEngine を指定します。

```bash
# Linux
FMEngineTest -e ./libDBOPLEngine.so patches/opl2.json
FMEngineTest -e ./libDBOPLEngine.so patches/opl3.json

# Windows
FMEngineTest.exe -e DBOPLEngine.dll patches/opl2.json
FMEngineTest.exe -e DBOPLEngine.dll patches/opl3.json
```

---

## API メモ

### FmEngine_AddChip の clock

`clock` はチップのマスタークロック (Hz) で、必ず指定します。0 は `FM_ERR_INVALID_ARG` です。

音程・エンベロープ・LFO の速さは、指定したクロックに比例します。
出力サンプルレートとの比が極端で扱えないクロック (例: 44,100 Hz で 1 Hz) も `FM_ERR_INVALID_ARG` です。

### FmEngine_Write の port 引数

| チップ | port=0                   | port=1                     |
|--------|--------------------------|----------------------------|
| OPL2   | 唯一のバス (0x388/0x389) | 無効 (FM_ERR_INVALID_ARG)  |
| OPL3   | Primary (0x388/0x389)    | Secondary (0x38A/0x38B)    |

OPL3 の Secondary アドレス空間 (0x100–0x1FF) は port=1 で書き込みます。  
OPL3 モード (reg 0x105 bit0 = NEW) が 0 の間、port=1 への書き込みは reg 0x05 を除いて port=0 と同じレジスタに書かれます。

### 部位ごとのゲイン

任意シンボル `FmEngine_SetPartGain` / `FmEngine_GetPartGain` / `FmEngine_GetPartMask` をエクスポートします。

| チップ | 部位 | 内容 | 既定値 |
|--------|------|------|--------|
| OPL, OPL2 | なし | ゲインは `FmEngine_SetGain` で設定 | — |
| OPL3 | `FM_PART_OPL3_AB` | 出力 A (L) / B (R) | 1.0 |
| OPL3 | `FM_PART_OPL3_CD` | 出力 C (L) / D (R) | 0 |

- 各チャンネルの出力先は reg 0xC0–0xC8 (両バンク) の bit4–7 (A, B, C, D) で選びます。NEW=1 の間、bit4–7 がすべて 0 のチャンネルはどの出力にも出ません (リズム音を除く)。
- NEW=0 の間は、出力先のビットに関係なく A/B に同じ音 (モノラル) を出し、C/D には何も出しません。
- リズム音 (reg 0xBD bit5) は、出力先のビットに関係なく A/B に出ます。`FM_PART_OPL3_CD` のゲインが 0 でなければ C/D にも出ます。

### FmEngine_SetMemory

OPL/OPL2/OPL3 は外部 ROM/RAM を持たないため、`FmEngine_SetMemory` は常に `FM_ERR_UNAVAILABLE`、`FmEngine_GetMemorySize` は常に 0 を返します。  
任意シンボル `FmEngine_SetMemoryEx` はエクスポートしません。

### OPL3 の有効化

`FmEngine_AddChip(engine, "OPL3", ...)` で追加した直後の OPL3 は、実機のリセット直後と同じく NEW=0 (OPL2 互換) です。  
18 チャンネル・4-op・ステレオなどの OPL3 の機能を使うには、port=1 で reg 0x05 に 0x01 を書き込みます。

```c
FmEngine_Write(engine, chip_id, 0x05, 0x01, 1);
```

### 出力レベル

dbopl の 16bit 相当の出力を 1/32768 倍して float にします。1 チャンネルを最大音量 (TL=0) の正弦波で鳴らしたときのピークは約 0.12 です。  
全チップを混ぜた出力は [-1.0, 1.0] に切り詰めます。範囲内の信号は変えません。

---

## ライセンス

このプロジェクトは **GNU General Public License v2.0** のもとで公開されています。詳細は [LICENSE](LICENSE) を参照してください。

`extern/dbopl/` は [rofl0r/dbopl](https://github.com/rofl0r/dbopl) を git submodule として参照しており、DOSBox Team により同じく GPL v2 で配布されています。
