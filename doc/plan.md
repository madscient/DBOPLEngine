# DBOPLEngine 作業計画・経緯

AI 向けの文書。決定の根拠と前提、見送った案、確かめた結果、未決事項、実行経緯を書く。
利用者向けの現在の仕様は `README.md` にある。

確度の印: **確認済み** = 走らせて確かめた（方法を添える）／**未検証** = 作ったが走らせていない／
**推測** = 出典を示せない（根拠を添える）。

## §0 現在地

- FmEngineApi の改訂（FMEngineTest `20c4923`: 部位と外部メモリを名前で指定する。ヘッダの正本が
  FMEngineTest の `include/FmEngineApi.h` になる）に追随した。部位は名前で受け取り（§2.11）、
  外部メモリの関数はどれもエクスポートしない（§2.5）
- それ以前に追随・修正したもの: 部位ゲイン（§2.1・§2.2）、port=1 が効いていない不具合（§2.6）、
  出力レベル（§2.8）、OPL3 の起動時の NEW（§2.9）、clock=0 の廃止（§2.10）
- この DLL は、`FmEngine_SetMemory` を必須シンボルとして読むアプリケーションからはロードできない
  （§2.5 の影響）。アプリケーション側が改訂に追随するまで、組み合わせて使えない
- 未決事項は無い。`FmEngine_GetNativeRate` は今のまま据え置く（§2.12）

## §1 仕様の出どころ

| 対象 | 正 | 時点 |
|---|---|---|
| API の仕様 | FMEngineTest `docs/FmEngineApi.md` | `20c4923` |
| ヘッダ | FMEngineTest `include/FmEngineApi.h`（`include/FmEngineApi.h` はその写し） | `20c4923` |

- 写しは写し元と同じ内容である。**確認済み**: `git hash-object include/FmEngineApi.h` が
  FMEngineTest の `20c4923:include/FmEngineApi.h` の blob（`206f723`）と一致する
- 改訂ごとにエンジンが追随すべき点は、FMEngineTest の `docs/CHANGELOG.md` に書いてある。
  `20c4923` では DBOPLEngine は「番号で指定する部位ゲインをエクスポートしている 7 本」と
  「外部メモリがスタブの 5 本」に入っている

## §2 決定

### 2.1 部位は OPL3 だけが持つ（`AB` と `CD`）

OPL / OPL2 は部位を持たない（`FmEngine_GetPartCount` が 0）。OPL3 は `AB` と `CD`。
既定値は AB = 1.0、CD = 0。

- 根拠: 仕様の「部位の名前」の表。表にあるチップに部位を持たせるエンジンは、名前と既定値を
  表のとおりにする。エンジンが選べる値は無い
- 前提: 仕様の表が変わらない限り成立する

### 2.2 出力 C/D は 2 つ目の dbopl で作る

dbopl は C0 の bit6/7（出力 C/D）を扱わない。`Channel::UpdateSynth` が bit4/5 から
`maskLeft` / `maskRight` を作るだけで、bit6/7 は読まない（`dbopl.cpp` を読んで確認）。

OPL3 のチップごとに dbopl をもう 1 つ持ち、全レジスタの書き込みを同じように流す。
C0 だけは bit6/7 を bit4/5 に付け替えて渡し（bit4/5 は捨てる）、その A/B 出力を C/D として
使う。CD のゲインが 0 の間も Generate して状態を揃えておく。

- 前提 1: dbopl は決定的である。`dbopl.cpp` に乱数の呼び出しは無く、ノイズは自前の LFSR
  （`noiseValue`）。grep で確認
- 前提 2: 出力先のマスクは合成に影響しない。`maskLeft` / `maskRight` は初期化・設定・
  出力への加算の 3 箇所でしか参照されない。grep で確認
- 前提 1・2 の帰結（同じ音を A/B に出した場合と C/D に出した場合で出力が一致する）は
  **確認済み**: `api_test` の 2-op・4-op・途中で CD ゲインを上げる項目で、出力の厳密一致を見た
- **submodule を更新したら前提 1・2 を見直す**
- 費用: OPL3 は 1 チップで dbopl を 2 つ動かす。CPU 負荷は計っていない（**未検証**）

### 2.3 NEW=0 の間、C/D には何も出さない

ymfm（YMEngine のコア）の `ch_output_2` / `ch_output_3` が NEW=0 のとき 0 を返すのに
合わせた。実機の挙動は確かめていない（**推測**: ymfm の実装だけが根拠）。

### 2.4 リズム音は C0 の出力先ビットを見ない（dbopl の制約）

dbopl の `GeneratePercussion<true>` は出力先に関係なく L/R の両方に足す。C/D 用の dbopl でも
同じになり、CD のゲインが 0 でなければリズム音は C/D にも出る。YMEngine（ymfm）は
ch6–8 の C0 に従うので、ここは食い違う。**確認済み**: `api_test` のリズムの項目。

### 2.5 外部メモリの関数はどれもエクスポートしない

仕様（`20c4923`）で、外部メモリの関数は任意の組になった。`FmEngine_GetMemoryCount` /
`FmEngine_GetMemoryName` / `FmEngine_SetMemory` / `FmEngine_SetMemoryEx` を、どれも定義しない。
仕様から無くなった `FmEngine_GetMemorySize` も無い。

- 根拠: 外部メモリを持つチップが無い。FMEngineTest の `docs/CHANGELOG.md`（`20c4923`）は、
  外部メモリがスタブのエンジン（DBOPLEngine を含む 5 本）にエクスポートをやめるよう書いている。
  呼び出し側は `FmEngine_GetMemoryCount` の無い DLL を、外部メモリを持たないエンジンとして扱う
- 仕様は、`FmEngine_GetMemoryCount` を持たずに `FmEngine_SetMemory` / `FmEngine_SetMemoryEx` を
  エクスポートする DLL を非互換としている（第 3 引数が文字列でない形と見分けられないため）。
  「一部だけ残す」形は取れない
- 前提: Y8950 や OPL4 のようにメモリを持つチップを足さない限り成立する
- 影響: `FmEngine_SetMemory` を必須シンボルとして読むアプリケーションは、この DLL のロードに
  失敗する。FitomEmuIF `75d9542` の `src/FmEmuIfImpl.cpp`（`LOAD_SYM` が無いシンボルで例外を投げる）
  と Y8960Sequencer `13050cb` の `src/core/fmengine.cpp`（`bind` が失敗を返す）がそうなっている。
  コードを読んで確かめた。組み合わせて走らせてはいない（**未検証**）。
  仕様の前提（番号で指定する形でビルドしたアプリケーションを、新しい形のエンジンと組み合わせて
  使わない）のとおりで、黙って誤動作するのではなくロードの時点で止まる
- やり直しの値段: 数 0 を返す形でエクスポートする（§3.9）なら、関数 3 つ・試験 1 項目・README

### 2.6 port を dbopl の `WriteAddr` に渡すとき 2 倍する

dbopl の `Chip::WriteAddr` は DOSBox の I/O ポート番号の下位 2 ビットを受け取り、
0（0x388）を primary、2（0x38A）を secondary として扱う。1 を渡すとアドレス 0 を返す。
旧版は port をそのまま渡していた。

- 影響: 旧版の OPL3 では port=1 の書き込みが全部 reg 0 に落ちていた。0x105 も届かず、
  常に NEW=0（OPL2 互換、9 ch、モノラル）で動いていた
- **確認済み**:
  - 旧版（`17b267f`）の DLL に当時の `api_test` を当てると、port=1・出力 A/B・
    「出力先ビット無しは無音」の項目が落ちる
  - FMEngineTest の `src/patches/opl3.json` を 44100 Hz で WAV に書き出した。旧版は全フレームで L=R、
    新版は L だけ・R だけの区間がある（パッチの pan=L / pan=R どおり）

### 2.7 ゲイン系の関数は `write_mutex` を取る

`FmEngine_Generate` が mutex の中でゲインを読むため。旧版の `FmEngine_SetGain` / `GetGain` は
mutex を取らずに書いており、オーディオスレッドとデータ競合していた。部位ゲインと揃えて直した。

### 2.8 出力の換算は 1/32768、ミックス後に [-1.0, 1.0] で切り詰める

旧版は `1 / (32768 * 256)` で、「dbopl は ±32768×256 程度を返す」という想定だった。実測では
TL=0 の正弦波 1 チャンネルのピークが 4072 で、dbopl の出力は 16bit の範囲を想定した値。
dbopl の `Handler::Generate` に残る無効化されたコードは、この int32 をそのまま DOSBox の
ミキサー（`AddSamples_m32`）に渡している。ユーザーの指示で 1/32768 にした。

1/32768 では全チャンネルを重ねると 1.0 を超えうるので、仕様の出力範囲に収めるため
全チップを混ぜた後に [-1.0, 1.0] で切り詰める。範囲内の信号は変えない。

- **確認済み**: FMEngineTest の `opl3.json` を 44100 Hz で WAV に書き出したときのレベル

  | エンジン | ピーク | RMS |
  |---|---|---|
  | YMEngine | -4.3 dBFS | -17.9 dBFS |
  | NukedEngine | -9.7 dBFS | -23.4 dBFS |
  | DBOPLEngine（旧版の換算） | -58.3 dBFS | -71.7 dBFS |
  | DBOPLEngine（1/32768） | -10.0 dBFS | -23.2 dBFS |

- **確認済み**: `api_test` の「1 チャンネルのピークが約 0.12」「18 チャンネルで 1.0 に切り詰める」の
  項目。前者は旧版の換算で落ち、後者は切り詰めを外した版で落ちる

### 2.9 `FmEngine_AddChip("OPL3")` では NEW を立てない

実機のリセット直後と同じく NEW=0（OPL2 互換）で始める。ユーザーの指示（実機に合わせる）。
YMEngine（ymfm）も NEW を立てない。旧版は README で自動で立てるとしていたが、§2.6 の不具合で
実際には効いていなかった。

- OPL3 の機能を使うアプリは port=1 で 0x05 に 0x01 を書く。FMEngineTest の `opl3.json` は書いている
- **確認済み**: `api_test` の「OPL3 は NEW=0 で始まる」の項目（自動で立てていた版で落ちる）、
  FMEngineTest の `opl3.json` の WAV で L だけ・R だけの区間がある

### 2.10 clock は必須にし、dbopl に渡すレートの換算で反映する

仕様（FMEngineTest `866f4a3`）: clock=0 は `FM_ERR_INVALID_ARG`。エンジンは既定のクロックを持たない。

旧版は clock を受け取って捨てていた。dbopl はクロックを受け取らず、内部レートを `OPLRATE`
（14318180 / 288 Hz）に固定してテーブルを作る。つまり旧版は、OPL / OPL2 を 3,579,545 Hz、
OPL3 を 14,318,180 Hz に固定した「既定のクロック」を持っていたのと同じだった。

dbopl の `Chip::Setup(rate)` は、周波数・エンベロープ・LFO・ノイズの刻みをすべて
`OPLRATE / rate` から作る（`dbopl.cpp` を読んで確認）。そこで、出力レートを
「dbopl が前提にしているクロック ÷ 指定クロック」倍したレートを dbopl の `Init` に渡す。
換算したレートが 0 か 32bit を超えるときは `FM_ERR_INVALID_ARG`。

- 前提: dbopl が時間に関わる値をすべて `Setup` のレートの比から作ること。submodule を更新したら見直す
- 誤差: 換算したレートは整数に丸める（相対誤差は 0.5 / レート、44100 Hz 付近で約 1e-5）。
  dbopl 自身も周波数の係数を整数に丸めている（`freqScale`）
- **確認済み**:
  - `api_test`: F-Number 0x244・BLOCK 4 が標準クロックで 440 Hz（ゼロ交差から求めた）、
    クロックを 2 倍・半分にすると音程が 2 倍・半分になる（OPL2・OPL3）
  - `api_test`: 2 倍のクロックを 44100 Hz で鳴らした出力が、標準クロックを 22050 Hz で鳴らした
    出力とサンプル単位で一致する。音色にビブラート・トレモロと減衰を入れてある。ただし、
    0.1 秒の区間で LFO と減衰が出力に表れていることは個別には確かめていない
  - `api_test`: 15 MHz の OPL3 で、C/D の出力が同じ音の A/B と一致する（C/D 用の dbopl も同じ換算で動く）
  - FMEngineTest `866f4a3` をビルドし、パッチのクロック（標準）で `opl2.json`・`opl3.json` を
    WAV に書き出した。clock を捨てていた版の出力とバイト単位で一致した

### 2.11 部位は名前で受け取り、チップ定義の表から引く

仕様（`20c4923`）: 部位は名前の文字列で指定し、`FmEngine_GetPartCount` /
`FmEngine_GetPartName` で列挙する。`FmPart` と `FmEngine_GetPartMask` は無くなった。

- チップ定義（`SupportedChipDef`）に部位の表（名前と既定値）を持たせ、列挙・名前の検索・
  既定値の設定はその表を引く。部位を持つチップを足すときは表を足す
- 名前は `strcmp` で比べる（大文字小文字を区別する。全体が一致したときだけ受け付ける）
- `index` の並びは AB、CD。仕様は並びを定めていないので、README には書いていない。
  前提: 呼び出し側が `index` に頼らないこと。並びを変えても仕様には反しない
- 名前の文字列は静的な定数を返す。仕様が求める寿命（`FmEngine_Destroy` が戻るまで）より長い
- 未知の chip_id は、`FmEngine_GetPartCount` が 0、`FmEngine_GetPartName` が nullptr。
  `part` が nullptr のときは `FM_ERR_INVALID_ARG`（どれも仕様のとおり）
- やり直しの値段: 名前や既定値は仕様の表で決まるので、変わるのは仕様が変わったとき。
  そのときは表の 1 行と試験・README

### 2.12 `FmEngine_GetNativeRate` は今のまま据え置く

出力サンプルレートを返す（dbopl が出力レートで直接生成するため）。クロックを変えても変わらない。
YMEngine は clock / 72（OPL / OPL2）を返すので、ここは食い違う。FMEngineTest の CHANGELOG
（`866f4a3`）は native_rate の変化で clock がエンジンに届いたことを確かめているが、DBOPLEngine では
この確かめ方が使えない（clock が届いていることは `api_test` の音程の項目で見ている。§2.10）。

- 根拠: ユーザーの判断（2026-10-03）。この関数は廃止予定なので、今は手を入れない
- 前提: 廃止の予定が変わらない限り成立する
- 仕様（`20c4923`）ではまだ必須シンボルなので、エクスポートは続ける。仕様から外れたら、
  その改訂への追随で外す

## §3 見送った案

- **3.1 OPL3 の部位を AB だけにする（C/D を作らない）。** 見送った理由: 仕様の表と食い違い、
  YMEngine と取り替えられなくなる。CPU 負荷（§2.2）が問題になるならこちら。
  戻す費用: 部位の表の 1 行、C/D 用 dbopl の削除、試験と README
- **3.2 C/D 用の dbopl を CD のゲインが 0 でない間だけ動かす。** ゲインを上げた時点で主の
  dbopl の `Chip` を値でコピーし、C0 を付け替える。既定（CD = 0）で CPU を増やさずに済む。
  見送った理由: `Chip` が自分自身を指すポインタを持たないこと（値でコピーできること）に依存する。
  現行の dbopl はコピーできる構造（`Operator::waveBase` は静的テーブルを指す）だが、
  単純さを優先した。CPU 負荷が問題になったら再検討する
- **3.3 dbopl を改造して C/D を出させる。** 見送った理由: submodule を改造しない
- **3.4 `FmEngine_SetMemoryEx` を、常に `FM_ERR_INVALID_ARG` を返す形で実装する。**
  見送った理由: 呼び出し側に何の情報も与えない（§2.5 で外部メモリの関数をすべて外した）
- **3.5 YMEngine と同じソフトクリップ（`x * (1 - x² / 9)`）を掛ける。** 見送った理由: 1.0 未満の
  信号も圧縮する（x = 0.5 で約 -0.25 dB）。仕様の範囲に収めるだけなら切り詰めで足りる
- **3.6 チップごとに出力 A–D を 16bit の範囲で飽和させてからゲインを掛ける**（NukedEngine は
  チップごとに 16bit で切り詰めている）。見送った理由: 1 チップをゲイン 1.0 で使う限り、ミックス後の
  切り詰めと結果が同じ。差が出るのはゲインが 1.0 でないときと複数チップのときだけ。
  入れる費用: Generate のチップごとのループに切り詰めを 1 つ足すだけ
- **3.7 標準クロック以外の clock を拒否する。** 見送った理由: 仕様の clock はマスタークロックで、
  dbopl に渡すレートの換算で反映できる。拒否すると、クロックの違う機種のパッチが使えない
- **3.8 clock=0 だけを弾き、0 以外は今までどおり捨てる。** 見送った理由: 固定のクロックを持つことになり、
  仕様の「エンジンは既定のクロックを持たない」に反する。クロックの違うパッチの音程が黙って狂う
- **3.9 外部メモリの 3 関数を、数 0 を返す形でエクスポートする**（`FmEngine_GetMemoryCount` が 0、
  `FmEngine_GetMemoryName` が nullptr、`FmEngine_SetMemory` が `FM_ERR_INVALID_ARG`）。これも仕様に
  準拠する。見送った理由: 呼び出し側から見て、シンボルが無いのと同じ（外部メモリを持たない）。
  FMEngineTest の CHANGELOG もスタブのエンジンにはエクスポートをやめるよう書いている。
  `FmEngine_SetMemory` を必須として読むアプリケーション（§2.5）がロードできるようにはなるが、
  そのアプリケーションは第 3 引数に番号を渡す
- **3.10 `FmEngine_SetMemory` だけを残す。** 見送った理由: 仕様が非互換としている形（§2.5）
- **3.11 `FmEngine_GetNativeRate` を、OPL / OPL2 は clock / 72、OPL3 は clock / 288（端数切り捨て）に
  する。** YMEngine と揃う。見送った理由: 廃止予定の関数（§2.12）。
  入れる費用: 関数 1 つ、試験の項目、README。生成そのものは変わらない

## §4 未決事項（ユーザーに確認する）

今は無い。

## §5 確かめた結果

### 現行（FMEngineTest `20c4923` への追随）

環境は Windows、MSVC 19.51（Visual Studio 18）、Ninja、Release。

- `api_test`: 全 51 項目 ok。`ctest` からも ok
- エクスポート: `dumpbin /exports` で `FmEngine_` のシンボルは 16 個（必須 12、部位 4）。
  外部メモリの 4 つ、`FmEngine_GetPartMask`、`FmEngine_GetMemorySize` は無い
- 変更前（`c67d84f`）の DLL に今の `api_test` を当てると落ちる項目: エクスポートの 3 項目、
  部位の各試験（`FmEngine_GetPartCount` が無いので飛ばしたことを失敗に数える）
- FMEngineTest `20c4923` をビルドし、`opl.json`・`opl2.json`・`opl3.json` を 44100 Hz で WAV に
  書き出した。変更前（`c67d84f`）の DLL の出力とバイト単位で一致する。無音ではない
  （`opl3.json` はピーク -10.0 dBFS、RMS -23.2 dBFS で §2.8 の表と同じ。L と R が違うフレームがある）。
  FMEngineTest は部位ゲインを呼ばないので、ここで見ているのは既定のゲインでの出力だけ
- FMEngineTest は、新旧どちらの DLL にも `FmEngine_GetMemoryCount is not exported` の断りを出して
  ロードする
- 変異を入れた DLL で落ちる項目（現行のソースから 29 通り作った。どの変異も 1 項目以上落ちる）:

  | 変異 | 落ちた項目 |
  |---|---|
  | 既定値を入れ替える（AB = 0、CD = 1.0） | 既定値、ほか 23 項目 |
  | 名前を大文字小文字を区別せずに比べる | 「大文字小文字を区別し、全体で一致」、ほか 2 項目 |
  | 名前を前方一致で比べる（部位の名前の長さで／引数の長さで、の 2 通り） | 同上 |
  | `part` の nullptr を調べない | 「nullptr を拒否」の項目の中で異常終了（0xC0000005） |
  | 出力先のポインタの nullptr を調べない | 同上 |
  | OPL2 に OPL3 の部位を持たせる | GetPartCount、GetPartName の範囲外、チップが持たない部位の拒否 |
  | GetPartCount が常に 2 を返す | GetPartCount |
  | GetPartName が範囲外で空文字列を返す | GetPartName の範囲外 |
  | GetPartName の並びが途中で入れ替わる | 同じ index に同じ名前 |
  | 部位の名前を変える（`CD` を別の名前に） | 名前の組、ほか 13 項目 |
  | 名前の検索が最後の部位を見ない | 「GetPartName が返した名前を受け付ける」、既定値、ほか 12 項目 |
  | SetPartGain が別の部位に書く | 設定して取得、ほか 12 項目 |
  | SetPartGain が全部の部位に書く | 「片方を設定しても他方は変わらない」、ほか 2 項目 |
  | 拒否した SetPartGain がゲインを書き換える | 「拒否した SetPartGain はゲインを変えない」、ほか 1 項目 |
  | AB のゲインを掛けない | チップゲイン × AB、NEW=0 で C/D から出ない |
  | `FmEngine_SetMemory` / `FmEngine_SetMemoryEx` / `FmEngine_GetMemoryCount` をエクスポートする（3 通り） | 外部メモリの関数をエクスポートしない |
  | `FmEngine_GetPartMask` / `FmEngine_GetMemorySize` をエクスポートする（2 通り） | API に無いシンボルをエクスポートしない |
  | `FmEngine_GetPartCount` をエクスポートしない | 部位 API のエクスポート、部位の各試験 |
  | C0 を付け替えない | C/D の一致、C のみ L / D のみ R、AB ゲインが C/D に届かない、チップゲイン × CD、途中で CD を上げる、15 MHz の OPL3 の C/D、4-op |
  | NEW=0 でも C/D を混ぜる | NEW=0 で C/D から出ない |
  | C/D にチップゲインを掛けない | チップゲイン × CD |
  | CD ゲインが 0 の間 C/D 用 dbopl を止める | 途中で CD を上げる |
  | ミックス後の切り詰めを外す | 1.0 に切り詰める |
  | C/D 用の dbopl だけ換算前のレートで初期化する | 15 MHz の OPL3 の C/D |
  | clock を捨てる（換算前のレートで初期化する） | 音程がクロックに追随する（OPL2・OPL3）、2 倍のクロックと半分のレートの一致（OPL2・OPL3）、15 MHz の OPL3 の C/D |

- Linux / macOS（GCC / Clang）ではビルドも試験もしていない（**未検証**）
- Visual Studio ジェネレータでの `ctest -C Release` は走らせていない（**未検証**）

### 以前の改訂のときの結果

- 旧版（`17b267f`）の DLL に当時の `api_test` を当てると落ちた項目: 部位ゲインのシンボル、port=1、
  出力 A のみ L、出力 B のみ R、出力先ビット無しは無音、部位の各試験。
  §2.8・§2.9 の前に当てた結果。§2.8 の後は換算の違いで可聴判定の項目も落ちる

## §6 経緯

- 2026-10-02: FmEngineApi の改訂（部位ゲイン、`FmEngine_SetMemoryEx`）に追随した。
  同時に port=1 の不具合を直し、`tests/api_test.cpp`・`CLAUDE.md`・この文書を足した
- 2026-10-02: ユーザーの判断で、出力の換算を 1/32768 にし（ミックス後に [-1.0, 1.0] で切り詰める）、
  OPL3 の起動時に NEW を立てないようにした
- 2026-10-02: FMEngineTest `c0589c1`（外部メモリの説明）と `866f4a3`（clock=0 の廃止）に追随した。
  コードを変えたのは後者だけ
- 2026-10-03: FMEngineTest `20c4923`（部位と外部メモリを名前で指定する、ヘッダの正本の移動）に
  追随した。ヘッダの写し元を YMEngine の `src/FmEngineApi.h` から FMEngineTest の
  `include/FmEngineApi.h` に替え、`CLAUDE.md` の規則も直した。部位は名前で受け取るようにし、
  `FmEngine_GetPartMask`・`FmEngine_SetMemory`・`FmEngine_GetMemorySize` のエクスポートをやめた。
  生成の計算は変えていない（§5 の WAV の一致）
- 2026-10-03: ユーザーの判断で、`FmEngine_GetNativeRate` は廃止予定のため据え置きにした（§2.12）
