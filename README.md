# RTL-sim — MC6809 RTL ベースシミュレーター（OS-9 Level 1 が動く）

Motorola MC6809E を、**命令単位の機能シミュレーションではなく RTL の流儀**（クロックエッジ・ノンブロッキング代入・バスサイクル単位）でモデル化した C++17 のシミュレーターです。sbc09（v09）互換のボード（ACIA／50Hz タイマ／ディスク）を載せ、**NitrOS-9 Level 1 が起動して `OS9:` プロンプトまで動作**します。

> 状態: Level 1 のみ対応。Level 2（MMU、`v09c` 相当）は未実装です。

## クイックスタート

```sh
make                        # build/rtlsim, build/ramtest, build/test_cpu
make check-cpu              # 指向テスト（外部ファイル不要）
scripts/setup_refs.sh       # sbc09 を clone して OS-9 ROM/ディスク/リファレンスをビルド（要ネットワーク）
build/rtlsim -rom build/os9v1.rom -0 build/disks/OS9.dsk -1 build/disks/WORK.dsk -v build/os9level1
```

`-v dir` はホストのディレクトリを OS-9 の `/v0` に見せます（例: `/v0/dir`, `/v0/mdir`）。端末が TTY なら対話入力、`-in file` ならスクリプト入力です。終了は `Ctrl-]`（`-e` で変更）。

主なオプション: `-rom -l -0 -1 -v -in -indelay N -cycles N -trace f -sched f -n N -tick N -fixed-time -firq -vcd f [-vcd-from N -vcd-cycles N] -iolog f -dumpram f -stats`。（`rtlsim` を引数なしで実行すると使い方が出ます。）

## RTL として何を再現しているか

| 機能シミュレーションに無い概念 | 本実装での扱い |
|---|---|
| クロックエッジ | `rtl/rtl.h`。MC6809E の直交クロック（Q が E より 90° 進む）。1 バスサイクル = 4 エッジ（QRise → ERise → QFall → EFall）。各モジュールは感度エッジを持つ |
| ノンブロッキング代入 | `Reg<T>` は `q`（見える値）と `d`（代入先）を持つ。プロセスは更新前の `q` だけを読み、エッジの最後に **全レジスタが一斉に `q←d`** される（Verilog の active → NBA 領域）。未代入のレジスタは保持。メモリ書き込みも `NbMem::nb_wr`（`mem[a] <= v`） |
| 組み合わせ回路 | NBA 後に `settle` で再評価（CPU 入力ピン `din/IRQ/FIRQ/NMI/HALT/RESET` など） |
| バスタイミング | E↓: CPU が次の アドレス/R/W/書込みデータ/VMA/LIC/BS/BA をレジスタ出力 → Q↑: チップセレクト確定（`sel` レジスタ）→ E↑: 選択デバイスが読み出しデータを出力（`rdata` レジスタ）→ E↓: CPU が `rdata` を取り込み、デバイスが書込み／読み出しストローブを見る |
| デバイス | ACIA・タイマ・ディスク制御はそれぞれ独立の Module。タイマは **E サイクルを数える**ので、シミュレーション時間はホスト速度に依存せず決定的 |
| 波形 | `-vcd` で E/Q/A/D/RW/VMA/LIC/BS/BA/割り込み線/内部レジスタ/μPC を VCD 出力（GTKWave） |

### CPU コアの構造（`rtl/mc6809*.{h,cpp}`）
マイクロプログラム方式です。

* **マイクロ ROM**（`mc6809_ucode.cpp`）: 1 マイクロ命令 = 1 バスサイクル（または長さ 0 のアクション）。命令ごとに「アドレッシング準備 → オペランド → 実行」の並びを起動時に組み立てます。インデックス指定は ポストバイトのクラス別ルーチン（間接指定は末尾が分岐）を共有します。
* **データパス**（`mc6809.cpp`）: 全フリップフロップは `Reg<Core>` 1 個。**EFall ごとに 1 回**、終了したバスサイクルのアクションを実行し、次のマイクロ命令を選び、次サイクルのバス要求を生成して `nb()` する。ガード（PSH/PUL ループ、LBcc の taken、CWAI/SYNC/RESET/HALT 待ち）で可変長シーケンスを表現。
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

## 検証

リファレンスはいずれも sbc09 の**生成コピー**（ビルド時にパッチ）です。v09s／engine.c には、データシートや MAME と食い違う不具合（DAA、SWI の E フラグ、PSHU/PULU の S、`NXORV`、$FFFF のラップ、TST の V、ADD の H、CWAI、vdisk の未初期化／範囲外読み出し 等）があるため、パッチ内容は `tools/ref/patch_*.py` に明記し、見つからないパターンはエラーにしています。

| テスト | 内容 | 結果 |
|---|---|---|
| `tests/run_lockstep.sh <seed> <n>` | ランダムな合法命令列を RTL と v09s で実行し、**命令ごとに pc/A/B/X/Y/U/S/DP/CC を比較**。サイクル数はデータシートモデルと比較 | 全シード一致（SYNC/不正命令に達した時点で打ち切り） |
| `build/test_cpu`（`make check-cpu`） | リセット列、IRQ/FIRQ/NMI/SWI/SWI2/SWI3 のスタックフレーム・ベクタ・19 サイクル、CWAI/SYNC/HALT、NMI のエッジ性と武装、DAA（BCD 100×100 全通り）、MUL（全 65536 通り） | 63 チェック合格 |
| `tests/os9_lockstep.sh` | OS-9 ROM を RTL と engine.c で起動し、タイマ割り込みの位置を RTL 側の記録から再生して命令ごとに比較。`/v0/dir`・`dir /d0`・`mdir`・`dir /d0/cmds` を含む | **3000 万命令**・割り込み 161 回で一致。コンソール出力とディスクイメージも同一 |

### 既知の注意点
* **タイマ周期の既定は 1,000,000 E サイクル**（`-tick`）。1 MHz で実時間 50Hz にあたる `-tick 20000` では、この sbc09 向け OS-9 ゲストが出力を化けさせます（`pty-dd.arm` など）。**原本の v09 でタイマを速めても同じ現象**を確認しており、RTL の不具合ではなくゲストの割り込み頻度への競合です。
* 実機の未定義挙動（ASL/ASR の H フラグ、未定義オペコード）は v09s に合わせています。実機と一致する保証はありません（知らんけど）。未定義オペコードのうち MAME が実装するものはデコードしますが、`ev_ill` イベントを立てます。
* ディスクは RTL 化していません（ホスト側モデル）。コマンド書き込み時に瞬時に完了します。

## ディレクトリ

```
rtl/    rtl.h（NBA/クロックカーネル）, mc6809.*（コア）, mc6809_ucode.cpp（マイクロ ROM 生成）
soc/    bus.h（Q/E バス）, devices.*（ROM/ACIA/タイマ/ディスク制御/I/O ページ）, sbc09.*（ボード）
host/   console.*（ターミナル/スクリプト入力）, vdisk.*（/v0 ホストファイル）, （ディスクは非 RTL）
tb/     rtlsim.cpp（本体）, ramtest.cpp（フラット RAM + トレース）, test_cpu.cpp, flatsys.h, vcd.h
tests/  gen_rand.py, run_lockstep.sh, os9_lockstep.sh, cmp_*.py, cycle_model.py, ram_bisect.py
tools/ref/  リファレンス・ハーネスとパッチ（sbc09 のソースは含まない）
scripts/setup_refs.sh   sbc09 の取得、a09 による OS-9 ROM 生成、ハーネスのビルド
```

## クレジットとライセンス

* 本リポジトリのコードは **MIT License**（`LICENSE`、© 2026 Shin'ichi KAYANUMA）です。
* **MAME m6809 コア**（© Nathan Woods, BSD-3-Clause）— バスサイクル列・割り込み手順などの仕様として参照。各ソース冒頭に明記。
* **sbc09**（© 1994 L.C. Benschop ほか sbc09 チーム、**GPL v2**。OS-9 対応版は Shinji Kono 氏の https://github.com/shinji-kono/sbc09）— `v09s.c`・`engine.c`・`io.c`・`vdisk.c`・`os9/` 以下（`vrbf.asm` 等）を参照／照合に使用。ソースは**コピーしておらず**、`scripts/setup_refs.sh` で別途取得します。
* `host/vdisk.cpp` は vdisk.c の**挙動（癖を含む）を再実装**したものです。GPL v2 のプログラムを忠実に模しているため、**派生物として扱う可能性があります**。再配布する場合は GPL v2 の適用可否を確認してください。
* **NitrOS-9 / OS-9**（Microware、NitrOS-9 プロジェクト）— ゲスト OS。ROM／ディスクイメージは本リポジトリに含めません（`setup_refs.sh` が sbc09 のソースからビルド）。
* Motorola MC6809/MC6809E データシート／プログラミングマニュアル — 命令表とサイクル表。
