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
│   └── FmEngineApi.h        ← FmEngineApi ファサードヘッダ (変更なし)
├── extern/
│   └── dbopl/               ← git submodule: https://github.com/rofl0r/dbopl
│       ├── dbopl.h
│       └── dbopl.cpp
└── src/
    └── DBOPLEngine.cpp      ← FmEngineApi 実装
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

### FmEngine_Write の port 引数

| チップ | port=0                   | port=1                     |
|--------|--------------------------|----------------------------|
| OPL2   | 唯一のバス (0x388/0x389) | 無効 (FM_ERR_INVALID_ARG)  |
| OPL3   | Primary (0x388/0x389)    | Secondary (0x38A/0x38B)    |

OPL3 の Secondary アドレス空間 (0x100–0x1FF) は port=1 で書き込みます。

### FmEngine_SetMemory

OPL2/OPL3 は外部 ROM/RAM を持たないため、常に `FM_ERR_UNAVAILABLE` を返します。

### OPL3 の有効化

`FmEngine_AddChip(engine, "OPL3", ...)` を呼ぶと、内部で自動的に  
`reg 0x105 = 0x01` (OPL3 enable bit) が設定されます。  
アプリケーション側での手動設定は不要です。

---

## ライセンス

このプロジェクトは **GNU General Public License v2.0** のもとで公開されています。詳細は [LICENSE](LICENSE) を参照してください。

`extern/dbopl/` は [rofl0r/dbopl](https://github.com/rofl0r/dbopl) を git submodule として参照しており、DOSBox Team により同じく GPL v2 で配布されています。
