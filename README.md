# RTL-sim — MC6809 RTL ベースシミュレーター（OS-9 Level 1 が動く）

Motorola MC6809E を、**命令単位の機能シミュレーションではなく RTL の流儀**（クロックエッジ・ノンブロッキング代入・バスサイクル単位）でモデル化した C11（C 言語）のシミュレーターです。sbc09（v09）互換のボード（ACIA／50Hz タイマ／ディスク）を載せ、**NitrOS-9 Level 1 が起動して `OS9:` プロンプトまで動作**します。

> 状態: **Level 1 と Level 2 の両方が起動します**（Level 2 は `-2`、sbc09 の `v09c` 相当の CoCo 風 MMU）。

## クイックスタート

```sh
make                        # build/rtlsim, build/ramtest, build/test_cpu
make check-cpu              # 指向テスト（外部ファイル不要）
scripts/setup_refs.sh       # sbc09 を clone して OS-9 ROM/ディスク/リファレンスをビルド（要ネットワーク）
build/rtlsim -rom build/os9v1.rom -0 build/disks/OS9.dsk -1 build/disks/WORK.dsk -v build/os9
```

`setup_refs.sh` は `scripts/build_cmds.sh` も呼び、NitrOS-9 のコマンド群（`dir copy list edit procs …`）と **BASIC09**（`basic09` / `runb`）を a09 でアセンブルして `build/os9/cmds` に置きます。SysGo は実行ディレクトリを `/v0/cmds` にするので、`-v build/os9` を付ければ名前だけで実行できます（例: `basic09`、`edit hello` → `run hello`）。標準の `OS9.dsk` は Dragon 用で BASIC09 などを含みません。

`-v dir` はホストのディレクトリを OS-9 の `/v0` に見せます（実行ディレクトリは `/v0/cmds`）。端末が TTY なら対話入力、`-in file` ならスクリプト入力です。終了は `Ctrl-]`（`-e` で変更）。

主なオプション: `-2`（Level 2 ボード）`-clock Hz`（既定 2000000）`-turbo` `-rom -l -0 -1 -v -in -indelay N -cycles N -trace f -sched f -n N -tick N -fixed-time -firq -vcd f [-vcd-from N -vcd-cycles N] -iolog f -dumpram f -stats`。（`rtlsim` を引数なしで実行すると使い方が出ます。）

## RTL として何を再現しているか

| 機能シミュレーションに無い概念 | 本実装での扱い |
|---|---|
| クロックエッジ | `rtl/rtl.h`。MC6809E の直交クロック（Q が E より 90° 進む）。1 バスサイクル = 4 エッジ（QRise → ERise → QFall → EFall）。各モジュールは感度エッジを持つ |
| ノンブロッキング代入 | `RTL_REG(T)` は `q`（見える値）と `d`（代入先）を持つ構造体。プロセスは更新前の `q` だけを読み、エッジの最後に **全レジスタが一斉に `q←d`** される（Verilog の active → NBA 領域）。未代入のレジスタは保持。メモリ書き込みも `rtl_nbmem_nb_wr`（`mem[a] <= v`） |
| 組み合わせ回路 | NBA 後に `settle` で再評価（CPU 入力ピン `din/IRQ/FIRQ/NMI/HALT/RESET` など） |
| バスタイミング | E↓: CPU が次の アドレス/R/W/書込みデータ/VMA/LIC/BS/BA をレジスタ出力 → Q↑: チップセレクト確定（`sel` レジスタ）→ E↑: 選択デバイスが読み出しデータを出力（`rdata` レジスタ）→ E↓: CPU が `rdata` を取り込み、デバイスが書込み／読み出しストローブを見る |
| デバイス | ACIA・タイマ・ディスク制御はそれぞれ独立の Module。タイマは **E サイクルを数える**ので、シミュレーション時間はホスト速度に依存せず決定的 |
| 波形 | `-vcd` で E/Q/A/D/RW/VMA/LIC/BS/BA/割り込み線/内部レジスタ/μPC を VCD 出力（GTKWave） |

### CPU コアの構造（`rtl/mc6809*.{h,c}`）
マイクロプログラム方式です。

* **マイクロ ROM**（`mc6809_ucode.c`）: 1 マイクロ命令 = 1 バスサイクル（または長さ 0 のアクション）。命令ごとに「アドレッシング準備 → オペランド → 実行」の並びを起動時に組み立てます。インデックス指定は ポストバイトのクラス別ルーチン（間接指定は末尾が分岐）を共有します。
* **データパス**（`mc6809.c`）: 全フリップフロップは `RTL_REG(McCore)` 1 個。**EFall ごとに 1 回**、終了したバスサイクルのアクションを実行し、次のマイクロ命令を選び、次サイクルのバス要求を生成して `RTL_NB` する。ガード（PSH/PUL ループ、LBcc の taken、CWAI/SYNC/RESET/HALT 待ち）で可変長シーケンスを表現。
* 命令境界で NMI → FIRQ → IRQ の優先順位判定。NMI は立ち下がりエッジ検出で、S への最初のロード（LDS／TFR→S／LEAS）後に有効化。
* ダミーサイクル（`VMA=0, A=$FFFF`）も含めてデータシートどおりのサイクル数。

## MAME ／ v09s との対応

| 参照元 | 何を参考にしたか |
|---|---|
| **MAME** `m6809.cpp / m6809.lst / base6x09.lst`（クロック／サイクル／CPU 状態の考え方） | バスサイクルの並び（`dummy_vma(n)` / `dummy_read_opcode_arg` 相当）、割り込み・CWAI・SYNC の手順（19 サイクル）、`m_lds_encountered`（NMI 武装）、NMI のエッジ検出、TFR/EXG の 8↔16 ビット規則、DAA アルゴリズム、未定義プレフィックスの扱い。**コードは写していません**（仕様として参照した再実装） |
| **sbc09 `v09s.c`**（命令仕様のリファレンス） | 命令の意味付け、未定義フラグの扱い（ASL の H は加算相当、ASR の H はオペランド bit4）。ランダム命令列の照合対象 |
| **sbc09 `engine.c / io.c / vdisk.c`**（shinji-kono/sbc09, OS-9 実行用） | I/O マップ（`$E000` ACIA、`$E030` タイマ/RTC、`$E040` ディスク）、pdisk のセクタ I/F、`/v0`（virtual RBF）コマンド・プロトコルとその癖。OS-9 起動の照合対象 |
| Motorola MC6809E データシート | サイクル表（独立した Python モデル `tests/cycle_model.py` で検証） |

メモリマップ（Level 1）: `$0000–ROM先頭-1` RAM、`ROM先頭–$FFFF` ROM（16KB イメージなら `$C000–`）、`$E000–$E1FF` I/O ページ（ROM に重なる。I/O レジスタ以外は ROM の中身が見える）。

## Level 2（MMU, `-2`）

```sh
build/rtlsim -2 -rom build/os9v2.rom -0 build/disks/OS9.dsk -1 build/disks/WORK.dsk -v build/os9l2
```

Level 2 のコマンド群と **BASIC09** は `LEVEL=2 scripts/build_cmds.sh`（`build/os9l2/cmds` に出力。`setup_refs.sh` の後に実行）で作ります。実行ディレクトリは自動では設定されないので、起動後に `chx /v0/cmds` してから `basic09` を実行します（BASIC09 の `MEM`・`DIR` などの応答を確認済み。実物の `v09c` と出力が一致）。

`v09c`（`-DUSE_MMU -DIOPAGE=0xff80`）のメモリモデルに合わせています。

| 項目 | 内容 |
|---|---|
| 物理メモリ | 2 MiB（8 KiB ブロック × 256）。ROM イメージは物理 `$7E000` から RAM に読み込む（書き込み保護なし） |
| 変換 | `Mmu`（`soc/devices.c`）。`$FF91` のタスクレジスタ bit0 でシステム用 `$FFA0–A7` / ユーザ用 `$FFA8–AF` を選択。変換は **Q↑ のバスデコード時に組み合わせ回路として**行い（`Bus.xlat`）、DAT への書き込みは NBA 後の次サイクルから有効 |
| 非変換領域 | `$FE00–$FFFF`（I/O ページ＋固定 RAM）。ACIA `$FF80/81`、MMU `$FF90–AF`、タイマ `$FFB0`、ディスク `$FFC0`。それ以外は物理 `$7FE00–` の RAM |
| レジスタの実体 | タイマ日時・ディスク・MMU レジスタは、v09c と同様に**物理 RAM と同じバイト**（`RegView` で RAM にエイリアス）。カーネルの `F$Move` が `$FFB1…` を物理メモリとして読むため必要（日付が `2155/255/255` になる不具合の原因だった） |
| vdisk | 要求フレーム／パス記述子はシステムマップ、名前・データバッファは呼び出しプロセスの DAT（プロセス記述子 `+$40` から取得）で変換。ディスクのセクタ DMA は現在のタスクマップ |

## 検証

リファレンスはいずれも sbc09 の**生成コピー**（ビルド時にパッチ）です。v09s／engine.c には、データシートや MAME と食い違う不具合（DAA、SWI の E フラグ、PSHU/PULU の S、`NXORV`、$FFFF のラップ、TST の V、ADD の H、CWAI、vdisk の未初期化／範囲外読み出し 等）があるため、パッチ内容は `tools/ref/patch_*.py` に明記し、見つからないパターンはエラーにしています。

| テスト | 内容 | 結果 |
|---|---|---|
| `tests/run_lockstep.sh <seed> <n>` | ランダムな合法命令列を RTL と v09s で実行し、**命令ごとに pc/A/B/X/Y/U/S/DP/CC を比較**。サイクル数はデータシートモデルと比較 | 全シード一致（SYNC/不正命令に達した時点で打ち切り） |
| `build/test_cpu`（`make check-cpu`） | リセット列、IRQ/FIRQ/NMI/SWI/SWI2/SWI3 のスタックフレーム・ベクタ・19 サイクル、CWAI/SYNC/HALT、NMI のエッジ性と武装、DAA（BCD 100×100 全通り）、MUL（全 65536 通り） | 63 チェック合格 |
| `LEVEL=2 tests/os9_lockstep.sh` | Level 2 ROM を RTL と `-DUSE_MMU` ビルドの engine.c（`refos9c`）で起動し、`dir /d0`・`mdir`・`dir /v0`・`dir /v0/cmds` を含めて比較 | **3000 万命令**・割り込み 155 回で一致。コンソール出力・ディスクも同一。実物の `v09c` とも `dir` 等の出力が一致（日時のみ異なる） |
| `tests/os9_lockstep.sh` | OS-9 ROM を RTL と engine.c で起動し、タイマ割り込みの位置を RTL 側の記録から再生して命令ごとに比較。`/v0/dir`・`dir /d0`・`mdir`・`dir /d0/cmds` を含む | **3000 万命令**・割り込み 161 回で一致。コンソール出力とディスクイメージも同一 |

### 動作クロック
`-clock Hz` で E クロックを指定します（既定 **2 MHz** = FM-11 EX の 68B09E 相当。FM-11 のメイン CPU は 2 MHz 駆動と記憶していますが、未確認です（知らんけど））。シミュレーション時間（サイクル数 ÷ クロック）をホストの `steady_clock` に 1 ms 刻みで同期させるので、**ホストが十分速ければ PC の性能に依存せず指定クロックで動作**します（この環境では約 6 MHz 相当まで出せます）。ホストが遅い場合は追いつけず遅れます（`-stats` に遅れたスライス数を表示）。テストでは `-turbo`（`-clock 0`）で制限なしに実行します。タイマ周期は `-tick`（サイクル数、既定 1,000,000 = 2 MHz で 2 Hz）で別指定です。

### 既知の注意点
* **タイマ周期の既定は 1,000,000 E サイクル**（`-tick`）。1 MHz で実時間 50Hz にあたる `-tick 20000` では、この sbc09 向け OS-9 ゲストが出力を化けさせます（`pty-dd.arm` など）。**原本の v09 でタイマを速めても同じ現象**を確認しており、RTL の不具合ではなくゲストの割り込み頻度への競合です。
* 実機の未定義挙動（ASL/ASR の H フラグ、未定義オペコード）は v09s に合わせています。実機と一致する保証はありません（知らんけど）。未定義オペコードのうち MAME が実装するものはデコードしますが、`ev_ill` イベントを立てます。
* ディスクは RTL 化していません（ホスト側モデル）。コマンド書き込み時に瞬時に完了します。

## ディレクトリ

```
rtl/    rtl.h（NBA/クロックカーネル）, mc6809.*（コア）, mc6809_ucode.c（マイクロ ROM 生成）
soc/    bus.h（Q/E バス）, devices.*（ROM/ACIA/タイマ/ディスク制御/I/O ページ）, sbc09.*（ボード）
host/   console.*（ターミナル/スクリプト入力）, vdisk.*（/v0 ホストファイル）, （ディスクは非 RTL）
tb/     rtlsim.c（本体）, ramtest.c（フラット RAM + トレース）, test_cpu.c, flatsys.h, vcd.h
tests/  gen_rand.py, run_lockstep.sh, os9_lockstep.sh, cmp_*.py, cycle_model.py, ram_bisect.py
tools/ref/  リファレンス・ハーネスとパッチ（sbc09 のソースは含まない）
scripts/setup_refs.sh   sbc09 の取得、a09 による OS-9 ROM（Level 1/2）生成、ハーネスのビルド
```

## クレジットとライセンス

* 本リポジトリのコードは原則 **MIT License**（`LICENSE`、© 2026 Shin'ichi KAYANUMA）です。**例外: `host/vdisk.c`・`host/vdisk.h` は GPL v2**（`LICENSES/GPL-2.0.txt`）で、これを含む `rtlsim` バイナリは全体が GPL v2 として扱われます。詳細は `NOTICE.md`。
* **MAME m6809 コア**（© Nathan Woods, BSD-3-Clause）— バスサイクル列・割り込み手順などの仕様として参照。各ソース冒頭に明記。
* **sbc09**（© 1994 L.C. Benschop ほか sbc09 チーム、**GPL v2**。OS-9 対応版は Shinji Kono 氏の https://github.com/shinji-kono/sbc09）— `v09s.c`・`engine.c`・`io.c`・`vdisk.c`・`os9/` 以下（`vrbf.asm` 等）を参照／照合に使用。ソースは**コピーしておらず**、`scripts/setup_refs.sh` で別途取得します。
* `host/vdisk.c` は vdisk.c の**挙動（癖を含む）を再実装**したもので、派生物として **GPL v2** を適用しています（上記）。
* **NitrOS-9 / OS-9**（Microware、NitrOS-9 プロジェクト）— ゲスト OS。ROM／ディスクイメージは本リポジトリに含めません（`setup_refs.sh` が sbc09 のソースからビルド）。
* Motorola MC6809/MC6809E データシート／プログラミングマニュアル — 命令表とサイクル表。
