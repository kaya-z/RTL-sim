# RTL-sim — MC6809 RTL ベースシミュレーター

Motorola MC6809E を、**命令単位の機能シミュレーションではなく RTL の流儀**（クロックエッジ・ノンブロッキング代入・バスサイクル単位）でモデル化した C11（C 言語）のシミュレーターです。sbc09（v09）互換のボード（ACIA／50Hz タイマ／ディスク）を載せ、**NitrOS-9 Level 1 / Level 2 が起動して `OS9:` プロンプトまで動作**し、BASIC09・YSD BASIC・**Pascal**・日本語ワープロ `jwp`・Dhrystone などのアプリも動きます。

## 動かせるもの

| 分類 | 内容 | Level 1 | Level 2 |
|---|---|---|---|
| OS | NitrOS-9（`OS9:` シェル、`dir` `copy` `list` など標準コマンド） | ○ | ○（CoCo 風 MMU・2 MiB） |
| 言語 | **BASIC09**（構造化 BASIC） | ○ | ○ |
| 言語 | **YSD BASIC**（`ysdbasic`、行番号つき BASIC） | ○ | ○ |
| 言語 | **Pascal**（`pc`。字句解析 → 構文解析 → 6809 コード生成 → アセンブル → OS-9 のモジュール。`string` `real` `set` ファイル `uses` など） | ×（メモリ不足） | ○ |
| エディタ | **jwp**（全画面の日本語ワープロ。汎用テキストエディタとしても使える。SKK 方式のかな漢字変換） | ○ | ○ |
| ベンチマーク | **Dhrystone**（`dhry`） | ○ | ○ |
| 終了 | `shutdown`（電源制御レジスタ） | ○ | ○ |

Pascal や jwp で「ソースを書く → コンパイルする → 実行する」まで、このシミュレータの中だけで完結します（手順は「アプリケーションの使い方」）。

## ビルドと起動手順

ROM イメージ・ディスクイメージ・コマンド群は本リポジトリに含まれません（ライセンスの都合）。**sbc09 のソースを取得して生成する手順（手順 2・4）が必要**です。

### 1. シミュレータのビルド

```sh
make                        # build/rtlsim, build/ramtest, build/test_cpu
make check-cpu              # 指向テスト（外部ファイル不要）
```

### 2. ROM・ディスク・Level 1 コマンドの生成（要ネットワーク、約 10 秒）

```sh
scripts/setup_refs.sh
```

`refs/sbc09`（shinji-kono/sbc09）を clone し、a09 で NitrOS-9 をアセンブルして次を作ります。

| 生成物 | 内容 |
|---|---|
| `build/os9v1.rom` | Level 1 の ROM イメージ |
| `build/os9v2.rom` | **Level 2 の ROM イメージ**（`-2` で使う） |
| `build/disks/OS9.dsk`, `WORK.dsk` | 256 バイト/セクタの生イメージ（`-0` / `-1`） |
| `build/os9/cmds/` | Level 1 のコマンド群と BASIC09（`/v0/cmds` として見せる） |
| `build/ref/` | 検証用の参照ハーネス（起動には不要） |

終了時に **`build/os9v1.rom` と `build/os9v2.rom` が存在すること**を確認してください（`ls build/*.rom`）。ROM は `scripts/build_rom.sh 1|2` が作ります（`setup_refs.sh` が呼びます。単独でも実行でき、失敗時は理由を表示し、a09 のログは `build/rom_modules.log`、リンクのログは `build/rom_link.log` に残ります）。`cannot read ROM image 'build/os9v2.rom'` と出る場合は、`scripts/build_rom.sh 2` を実行してください。以前の版の `setup_refs.sh` で作った `refs/` が残っている場合も、これで ROM が作られます（`LEVEL=2 scripts/build_cmds.sh` は Level 2 の ROM が無ければ自動で作ります）。

### 3. Level 1 の起動

```sh
build/rtlsim -rom build/os9v1.rom -0 build/disks/OS9.dsk -1 build/disks/WORK.dsk -v build/os9
```

`OS9:` プロンプトが出れば起動完了です。`-v build/os9` は、ホストの `build/os9` ディレクトリを OS-9 の `/v0` に見せます。SysGo が実行ディレクトリを `/v0/cmds` にするので、コマンドは名前だけで実行できます（`dir`、`procs`、`basic09` など）。端末が TTY なら対話入力、`-in file` ならスクリプト入力です。**終了は `Ctrl-]`**（`-e` で変更）。

### 4. Level 2 の起動

Level 2 では、ROM とコマンド群を Level 2 用に用意します（`setup_refs.sh` の後。`setup_refs.sh` が作る `build/os9v2.rom` が無い場合も、2 行目で作られます）。

```sh
ls build/os9v2.rom || scripts/build_rom.sh 2   # Level 2 の ROM。無ければここで作る（失敗時は理由を表示）
LEVEL=2 scripts/build_cmds.sh                  # build/os9l2/cmds に出力（BASIC09 を含む）
build/rtlsim -2 -rom build/os9v2.rom -0 build/disks/OS9.dsk -1 build/disks/WORK.dsk -v build/os9l2
```

`build_cmds.sh` の最後に出る `not assembled: …` は、CoCo のグラフィックドライバ・SCSI・フォーマットなどハードウェア依存のソースで、**エラーではありません**（不要）。最終行の `essential programs: … basic09:ok runb:ok …` がすべて `ok` ならコマンド群は揃っています。

`-2`（Level 2 ボード）・`build/os9v2.rom`・`-v build/os9l2` の 3 つがそろって初めて動きます。Level 1 の ROM に `-2` を付けたり、Level 2 の ROM を `-2` なしで起動したりしないでください。

Level 2 では実行ディレクトリが自動設定されないため、起動後に最初に次を入力します。

```
OS9:chx /v0/cmds
```

### 5. 全アプリ入りで起動する（jwp・Dhrystone・YSD BASIC・BASIC09）

[kaya-z/sim09](https://github.com/kaya-z/sim09) の OS-9 用アプリ（日本語ワープロ `jwp`、Dhrystone、YSD BASIC）は、日本語対応に改造した SCF（`kmode` コマンドで文字コードを指定）を前提にするため、sim09 の **ROM とディスクをそのまま使います**。取得と配置は次のスクリプトです（要ネットワーク。手順 2 の後）。

```sh
scripts/setup_jwp.sh        # refs/sim09 を取得し、build/jwp/ に ROM・ディスクを置く
sudo apt install skkdic     # かな漢字変換の辞書（SKK-JISYO.L）。入れてから上を再実行すると SKK_utf8.dsk ができる
```

```sh
# Level 1（BASIC09 は手順 2 の build/os9 から /v0/cmds として使う）
build/rtlsim    -rom build/jwp/os9v1.rom -0 build/jwp/OS9.dsk   -1 build/jwp/SKK_utf8.dsk -v build/os9
# Level 2（BASIC09 は手順 4 の build/os9l2 から）
build/rtlsim -2 -rom build/jwp/os9v2.rom -0 build/jwp/OS9L2.dsk -1 build/jwp/SKK_utf8.dsk -v build/os9l2
```

| 場所 | 中身 |
|---|---|
| `/d0/cmds`（`OS9.dsk` / `OS9L2.dsk`） | 標準コマンド、`jwp`、`dhry`、`ysdbasic`、`shutdown`、`pc`（Pascal。**Level 2 のみ**） |
| `/d0/PAS`・`/d0/LIB`（`OS9L2.dsk`） | Pascal のサンプルプログラム・ライブラリ |
| `/v0/cmds`（ホストの `build/os9` / `build/os9l2`） | `basic09`、`runb` と、手順 2・4 でビルドしたコマンド |
| `/d1`（`SKK_utf8.dsk`） | SKK 辞書 `skk.dic`、学習結果 `skkuser.dic`、作業用のファイル置き場 |

* ROM は `build/jwp/` のものを使ってください。本リポジトリで作った `build/os9v1.rom` / `os9v2.rom`（標準の SCF）では、`jwp` は 1 バイト文字モードになり、日本語の編集と変換が使えません。
* 辞書が無くても `jwp` は使えます（`-1 build/jwp/WORK.dsk`）。**ディスクイメージは書き換わる**ので、残したくなければコピーを使ってください。
* 端末は **UTF-8 の ANSI（VT100 系）端末**で、80×24 以上にしてください。

## アプリケーションの使い方

起動したら最初に、使うコマンドのあるディレクトリを実行ディレクトリにします（既定は `/v0/cmds`）。

```
OS9:chx /d0/cmds        jwp / dhry / ysdbasic / shutdown などはこちら
OS9:chx /v0/cmds        basic09 / runb はこちら（-v を付けたとき）
```

| やりたいこと | コマンド | 節 |
|---|---|---|
| 文章・ソース・メモを書く（エディタ） | `jwp ファイル名` | 下の「jwp」 |
| BASIC でプログラムを書いて走らせる | `ysdbasic`（行番号つき）/ `basic09`（構造化） | 下の各節 |
| Pascal でプログラムを書いて走らせる（Level 2） | `pc ファイル.pas` | 下の「Pascal」 |
| CPU 性能を測る | `dhry` | 下の「Dhrystone」 |
| 終了する | `shutdown`（または `Ctrl-]`） | 下の「終了」 |

### OS-9 シェルの基本

| コマンド | 内容 |
|---|---|
| `dir` / `dir /d0/cmds` | ディレクトリの一覧（`dir /d1` は作業ディスク） |
| `list ファイル` | テキストファイルを表示 |
| `copy 元 先` | ファイルのコピー（`copy /d0/cmds/echo /d1/work/a`） |
| `del ファイル` / `makdir 名前` | 削除（`chd` したディレクトリ内の名前、またはパス）/ ディレクトリ作成（`makdir /d1/work`） |
| `date` | 日時 |
| `mfree` / `procs` / `mdir -e` | 空きメモリ / プロセス一覧 / メモリ上のモジュール一覧 |
| `chx ディレクトリ` / `chd ディレクトリ` | 実行ディレクトリ / データディレクトリの変更 |

`OS9.dsk` は `/d0`、`WORK.dsk` や辞書ディスクは `/d1`、ホストのディレクトリは `/v0` です。

### jwp（エディタとして使う）

`jwp` は日本語ワープロですが、**OS-9 の汎用テキストエディタ**としても使えます。OS-9 には行エディタの `edit` しか無く、全画面で編集できるものはこれだけです。OS-9 のテキストファイルは行末が CR で、`jwp` は改行形式（CR / LF / CRLF）を自動判定して、同じ形式で書き戻します。

```
OS9:chx /d0/cmds
OS9:jwp /d1/hello.bas              ファイルを開く（なければ新規作成）
10 FOR I=1 TO 3: PRINT I*I: NEXT   そのまま打つ。Enter で改行
20 PRINT "END"
^S                                 保存（Ctrl-S）
^Q                                 終了（変更があれば確認される）
OS9:list /d1/hello.bas             保存された内容を確認
```

書いたファイルは他のアプリに渡せます（下の例は Level 2 で確認。Level 1 の注意は最後の節）。

```
OS9:ysdbasic /d1/hello.bas         → 1 4 9 END と表示される
OS9:copy /d1/hello.bas /d1/copy.bas
```

主なキー（全部は sim09 の `docs/JWP.md`）:

| キー | 動作 | キー | 動作 |
|---|---|---|---|
| `←` `→` `↑` `↓` | カーソル移動 | `^S` / `^W` | 保存 / 名前を付けて保存 |
| `^A` `^E` | 行頭 / 行末 | `^O` | 別のファイルを開く |
| `PgUp` `PgDn` | 1 画面 上 / 下 | `^R` / `^T` | 検索 / 次を検索 |
| `BS` `^D` | 左 / カーソル位置の 1 文字を削除 | `^K` / `^Y` | 行末まで切り取り / 貼り付け |
| `Tab` | 全角空白（字下げ） | `^X` | 整形出力（折り返しを実際の改行にしたファイル） |
| `^L` | 画面の描き直し | `^Q` | 終了 |

* 起動オプション: `jwp -w 60 -h 20 ファイル`（窓の幅・高さ）、`-d 辞書` `-u ユーザ辞書`、`-v`（版の表示）。
* 日本語は端末の IME で入力できます。IME を使わずに入力するには SKK 方式の変換を使います。`^J` でかなモード（ローマ字がひらがなになる。`l` で英数に戻る）、読みを大文字で始めて Space で変換、Enter で確定です。例: `Kanji` + Space → 漢字、`Nihongo` + Space → 日本語（Level 1・Level 2 とも確認）。
* 文書サイズの上限は Level 1 が約 9000 バイト、Level 2 が約 26000 バイトです（空き RAM による）。
* `-in script.txt` で入力を流し込む場合、LF（`^J`）は CR に変換されるため、かなモードの `^J` は送れません。スクリプトで変換を試すときは標準入力から流します（例: `(sleep 16; printf 'chx /d0/cmds\r'; …; printf '\nKanji ') | build/rtlsim …`）。

### BASIC09

```
OS9:chx /v0/cmds
OS9:basic09                  ← B: プロンプトになる
B:EDIT hello                 ← エディタ（E: プロンプト）。行番号付きで入力する
E:10 FOR I=1 TO 3
E:20 PRINT I*I
E:30 NEXT I
E:40 PRINT 6*7
E:q                          ← エディタを終了
B:RUN hello                  ← 1. 4. 9. 42 と表示される
B:MEM                        ← 空きメモリ。DIR はプロシージャ一覧
B:BYE                        ← OS9: に戻る
```

* B: プロンプトに式（`PRINT 6*7` など）を直接入力しても `What?` になります（BASIC09 の仕様で、実物の `v09c` でも同じ）。
* BASIC09 のプロシージャは中間コードで、`jwp` では編集できません（BASIC09 の `EDIT` を使います）。
* Level 1・Level 2 とも `EDIT hello` → `RUN hello` まで動作確認済みです（Level 2 は実物の `v09c` と出力が一致）。
* 入力を一度に流し込む場合は `-in script.txt -indelay 30000000`（起動完了まで入力を保留）を使います。

### YSD BASIC（`ysdbasic`）

行番号つきの小さな BASIC（1980 年代の Microsoft 系 BASIC に近い。BASIC09 とは別物。32 ビット浮動小数点をソフトウェアで計算）です。プログラムは `jwp` で書いても、その場で打ってもかまいません。

```
OS9:chx /d0/cmds
OS9:ysdbasic                          対話モード。Ok が出たら 1 行ずつ打つ（行番号つきはプログラムに入り、なしは即実行）
Ok
10 FOR I=1 TO 3: PRINT I*I: NEXT
20 PRINT SQR(2), SIN(1), "HI"+STR$(42)
RUN                                   → 1 4 9 と、1.41421  0.841471  HI 42
SAVE "/d1/t.bas"                      OS-9 のテキストファイルとして保存（jwp で開ける）
BYE                                   OS9: に戻る
OS9:ysdbasic /d1/t.bas                ファイルを読み込んで実行し、シェルに戻る
```

Level 1・Level 2 とも、上の例が sim09 の文書どおりの出力になることを確認しました（空き領域は Level 1 が 3600 バイト、Level 2 が 12000 バイト）。

### Pascal（`pc`、Level 2 のみ）

[sim09](https://github.com/kaya-z/sim09) に入っている MK09 の **Pascal コンパイラ**の OS-9 版です。`pc` が `pas0`（字句解析）→ `pas1`（構文解析）→ `cg`（6809 のコード生成）→ `as`（アセンブラ）を順に動かして、**OS-9 のモジュール（普通に実行できるコマンド）**を作ります。標準 Pascal の中核に、Turbo Pascal の `string`・`real`・`longint`・`set`・ファイル・`uses`・`crt` の一部を足した言語です。コンパイラが 64 KiB の空間をほぼ使うため **Level 1 では動きません**（Level 2 の `OS9L2.dsk` に入っています）。

```
OS9:chx /d0/cmds
OS9:chd /d0/PAS                  サンプルのディレクトリ（コンパイルの出力もここにできる）
OS9:dir
calc.pas   hanoi.pas   hello.pas   primes.pas
OS9:pc hello.pas                 → hello: 1611 bytes, 35 relocations, bss 609
OS9:chx /d0/PAS                  できたモジュールのあるディレクトリを実行ディレクトリに
OS9:hello                        → Hello from Pascal on OS-9!
```

自分のプログラムは `jwp` で書けます（**エディタ → コンパイル → 実行**。Level 2 で確認）。Pascal の文字列は `'` で囲みます。

```
OS9:chx /d0/cmds
OS9:chd /d1                      作業ディスクにソースと出力を置く
OS9:jwp sq.pas                   program sq;
                                 var i: integer;
                                 begin
                                   for i := 1 to 5 do writeln(i, ' squared = ', i*i)
                                 end.
                                 （^S で保存、^Q で終了）
OS9:pc sq.pas                    → sq: 4130 bytes, 73 relocations, bss 617
OS9:chx /d1
OS9:sq                           → 1 squared = 1 … 5 squared = 25
```

* 確認したサンプル: `hanoi`（4 枚で 15 手）、`primes`（100 まで 25 個）、`calc`（`3 4.5 10 0` を入力すると `count 3` `sum 17.50` `mean 5.83` `max 10.00`）。いずれも出力が正しいことを確認しました。
* `pc [-S] [-k] [-V] [-v] [-o 出力] ファイル.pas`。`-S` はアセンブラの文（`.s`）で止める、`-k` は途中のファイルを残す、`-V` は各段のコマンドを表示します。途中のファイル（`_t` `_i` `_a`）は今のデータディレクトリに作られ、終われば消えます。
* できたモジュールは普通の OS-9 のコマンドで、入出力のリダイレクト（`hello >out.txt`、`calc <data.txt`）もパイプも使えます。引数は `paramstr(1)` … で読めます。`uses` や `{$I …}` のファイルは、プログラムと同じ場所、今のディレクトリ、`/d0/LIB` の順に探します。
* 時間: sim09 の文書では、シミュレータの時計で `hello.pas` が約 14 秒、200 行ほどの `calc.pas` が約 55 秒です。実時間（既定 2 MHz）で待たずに済ませるには `-turbo` を付けます。
* 制限: できるモジュールの大きさ（`dir e` で見える大きさ）は約 28 KB までで、これを超えるとコンパイルできても動きません。名前は 15 文字まで（識別には 12 文字）、範囲検査と可変レコードはありません。詳しくは sim09 の `docs/OS9_MANUAL.md` の 9.5 節と `mk09/docs/PASCAL.md`。
* **`pc` が見つからない（`Error #255`）ときの確認**:
  1. **Level 2 で起動しているか**: `-2 -rom build/jwp/os9v2.rom -0 build/jwp/OS9L2.dsk`。Level 1（`OS9.dsk`）には `pc` はありません。
  2. **`chx /d0/cmds`** を入力したか（既定の実行ディレクトリは `/v0/cmds`）。`dir /d0/cmds` に `pc` `pas0` `pas1` `cg` `as` が並んでいれば、ディスクには入っています。
  3. **ディスクが古くないか**: Pascal は sim09 に後から追加されました。`scripts/setup_jwp.sh` を実行し直してください（`refs/sim09` を最新に更新し、`build/jwp/` の ROM・ディスクを置き直します。末尾に `OS9L2.dsk: pas1 ok` などと出れば入っています。`MISSING` / `WARNING` が出たら古いままです）。**ディスクイメージは上書きされる**ので、作ったファイルを残したいときは先に取り出してください。
  4. `-v` で別のディレクトリを `/v0` にしていても、`pc` は `/d0/cmds` にあります（`/v0/cmds` ではありません）。
* 実行ディレクトリが `/d0/PAS` のままだと `shutdown` が見つからず `Error #216` になります。`chx /d0/cmds` に戻してから実行してください。

### Dhrystone（`dhry`）

```
OS9:chx /d0/cmds
OS9:dhry 5000          5000 回だけ回して、最終値の検査（Dhrystone checks OK）と所要秒数を出す
OS9:dhry               10 秒以上（OS-9 の時間で）回して Dhrystones/s を出す
```

OS-9 の時計は 1 秒単位で、Clock モジュールは**毎秒 60 ティック**を前提にしています。タイマが 50 Hz だと OS-9 の 1 秒は実時間の 1.2 秒になるので、`dhry` 自身の表示は正確ではありません。サイクル数で測るには、`dhry 1000` と `dhry 4000` を別々に実行して差を取ります（起動とシェルの分が消えます）。出力に指定の文字列が出た時点で止まり、E サイクル数を表示する `-until` を使います。

```sh
for n in 1000 4000; do
  printf "chx /d0/cmds\rdhry $n\r" > /tmp/d.in
  cp build/jwp/OS9.dsk /tmp/d0; cp build/jwp/WORK.dsk /tmp/d1
  build/rtlsim -rom build/jwp/os9v1.rom -0 /tmp/d0 -1 /tmp/d1 -in /tmp/d.in -indelay 30000000 -noterm -fixed-time \
      -tick 36864 -turbo -until "clock counts seconds)" -cycles 900000000 2>&1 | grep until
done          # 差 ÷ 3000 = 1 回あたりのサイクル数（-tick 36864 は 1.8432 MHz の 50 Hz）
```

| | 1 回あたり（本シミュレータ） | sim09 の文書の値 | Dhrystones/s（1.8432 MHz） |
|---|---|---|---|
| Level 1 | **8766 サイクル** | 8766 | 210.3 |
| Level 2 | **8853 サイクル** | 8852 | 208.2 |

CPU の実装も I/O の実装も別物の sim09 と、1 回あたりのサイクル数が一致しました（Level 1 は整数部まで、Level 2 は 1 サイクル差）。ベンチマーク全体（タイマ割り込みとスケジューラを含む OS-9 の動作）でのサイクル精度の裏付けになります。Dhrystones/s はクロックに比例します（2 MHz なら約 228）。

### 終了（`shutdown`）

```
OS9:shutdown
System halted.
```

`shutdown` は I/O ページ基準の `+$0F`（Level 1 は `$E00F`、Level 2 は `$FF8F`）の電源制御レジスタに `$A5` を書きます。本シミュレータはこれを電源断として扱い、`[rtlsim] power off (shutdown) at E-cycle N` と表示して**正常終了（終了コード 0）**します（`-in` のスクリプトの最後に `shutdown` を入れておくと、`-cycles` の上限を決めずに自動で終われます）。`Ctrl-]` で抜けることもできます。ディスクへの書き込みは 1 回ごとに完了するので、`shutdown` を忘れても通常は壊れません。編集中のファイルは、先に保存して `jwp` を終了してから `shutdown` します。

### Level 1 でアプリを切り替えるとき（メモリ不足 `ERROR #207`）

Level 1 は 48 KiB しか RAM が無く、一度実行したコマンドのモジュールがメモリに残ります（`unlink` しても空きは戻りません）。このため、`jwp` で編集したあとに `ysdbasic` を実行すると `ERROR #207`（メモリ不足）になります。**`shutdown` して、`rtlsim` を起動し直してから次のアプリを実行してください**（ディスクイメージにファイルは残ります）。`jwp`・`ysdbasic` は起動直後の最初のコマンドとして実行するのが確実です。これは sim09 本体でも同じページ数・同じエラーになるゲスト OS-9 の挙動で、シミュレータの不具合ではありません。Level 2 はプロセスごとに 64 KiB なので起きません。

## 主なオプション

`-2`（Level 2 ボード）`-clock Hz`（既定 2000000）`-turbo` `-rom -l -0 -1 -v -in -indelay N -cycles N -trace f -sched f -n N -tick N -tickhz F -fixed-time -firq -vcd f [-vcd-from N -vcd-cycles N] -iolog f -dumpram f -until 文字列 -stats`。（`rtlsim` を引数なしで実行すると使い方が出ます。）

## メモリマップ

### I/O レジスタ（共通。基準アドレス: Level 1 は `$E000`、Level 2 は `$FF80`）

| 基準からのオフセット | Level 1 | Level 2 | 内容 |
|---|---|---|---|
| `+$00` / `+$01` | `$E000` / `$E001` | `$FF80` / `$FF81` | ACIA 状態（bit0 受信あり、bit1 送信可）/ データ |
| `+$0F` | `$E00F` | `$FF8F` | 電源制御（`$A5` を書くと停止。`shutdown` が使う） |
| `+$10`〜`+$2F` | （なし） | `$FF90`〜`$FFAF` | MMU（`$FF91` タスク、`$FFA0–A7` システム DAT、`$FFA8–AF` ユーザ DAT）。`$FF90` は汎用 |
| `+$30` | `$E030` | `$FFB0` | タイマ制御。書込み `$8F` 開始 / `$80` 停止 / `$04` 日時ラッチ。読出し bit4 = ティック |
| `+$31`〜`+$36` | `$E031`〜 | `$FFB1`〜 | 日時 年(1900 からの経過)・月・日・時・分・秒 |
| `+$40` | `$E040` | `$FFC0` | ディスク コマンド/状態。`$81` 読出し `$55` 書込み、`$D1`〜`$DD` は vdisk（/v0）。状態 `0`=正常 `$FF`=エラー |
| `+$41`〜`+$44` | `$E041`〜 | `$FFC1`〜 | ドライブ番号、LSN（3 バイト）。vdisk では `+$44` がカレントディレクトリ番号 |
| `+$45`〜`+$48` | `$E045`〜 | `$FFC5`〜 | バッファアドレス（`+$45/46`）。vdisk では呼び出しフレーム（`+$45/46`）とパス記述子（`+$47/48`） |

### Level 1（64 KiB フラット。ROM は 16 KiB イメージなら `$C000` から）

| CPU アドレス | 内容 |
|---|---|
| `$0000`〜ROM先頭−1 | RAM（既定 `$0000–$BFFF`） |
| ROM先頭〜`$FFFF` | ROM（既定 `$C000–$FFFF`。書込みは無視。ベクタ `$FFF2–$FFFF`） |
| `$E000`〜`$E1FF` | I/O ページ（ROM に重なる）。上表のレジスタ以外の読出しは ROM の中身、`$E050` 以降への書込みはページ内に保持される |

### Level 2（CPU 64 KiB を MMU で 2 MiB の物理メモリに写像）

CPU から見た 64 KiB は 8 KiB × 8 ブロックで、DAT の 1 バイトが物理ブロック番号（0〜255）です。

| CPU アドレス | 内容 |
|---|---|
| `$0000`〜`$FDFF` | MMU で変換される領域（DAT 経由で物理 RAM へ） |
| `$FE00`〜`$FF7F` | 固定 RAM（**変換されない**。物理 `$7FE00`〜に固定） |
| `$FF80`〜`$FFFF` | I/O レジスタ（上表）と固定 RAM。`$FFF0–$FFFF` は割り込みベクタ（ROM イメージの内容） |

| 物理アドレス | 内容 |
|---|---|
| `$000000`〜`$1FFFFF` | 2 MiB RAM（ROM も RAM。書込み保護なし） |
| `$07E000`〜 | 起動時に ROM イメージ（`os9v2.rom`）を配置。リセット直後は システム DAT の第 7 ブロックだけが有効（`$3F`、物理 `$7E000`）で、他の 7 ブロックは物理ブロック 0 を指す |
| `$07FE00`〜`$07FFFF` | 固定 RAM / I/O ページの実体（タイマ日時・ディスク・MMU レジスタのバイトもここ） |

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


## Level 2（MMU, `-2`）の実装

（起動手順は上の「ビルドと起動手順」を参照）

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
| `tests/alu_exhaustive.py`（`make check`） | ALU を v09s と**網羅比較**: 8 ビット二項演算 9 種 × 全オペランド対 × キャリー（A 形式）、単項演算・DAA・SEX × 全値 × CC 入力 6 ビット全通り、16 ビット演算（ADDD/SUBD/CMPD/CMPX/CMPY/CMPU/CMPS）は境界値 20 点の総当たり、MUL 全 65536 通り、ABX。DAA にバグを仕込んで検出されることも確認 | **187 万ケース、不一致 0** |
| `build/test_cpu`（`make check-cpu`） | リセット列、IRQ/FIRQ/NMI/SWI/SWI2/SWI3 のスタックフレーム・ベクタ・19 サイクル、CWAI/SYNC/HALT、NMI のエッジ性と武装、DAA（BCD 100×100 全通り）、MUL（全 65536 通り） | 63 チェック合格 |
| `LEVEL=2 tests/os9_lockstep.sh` | Level 2 ROM を RTL と `-DUSE_MMU` ビルドの engine.c（`refos9c`）で起動し、`dir /d0`・`mdir`・`dir /v0`・`dir /v0/cmds` を含めて比較 | **3000 万命令**・割り込み 155 回で一致。コンソール出力・ディスクも同一。実物の `v09c` とも `dir` 等の出力が一致（日時のみ異なる） |
| `tests/os9_lockstep.sh` | OS-9 ROM を RTL と engine.c で起動し、タイマ割り込みの位置を RTL 側の記録から再生して命令ごとに比較。`/v0/dir`・`dir /d0`・`mdir`・`dir /d0/cmds` を含む | **3000 万命令**・割り込み 161 回で一致。コンソール出力とディスクイメージも同一 |

### 動作クロック
`-clock Hz` で E クロックを指定します（既定 **2 MHz**。68B09**E** の 2 MHz は外部クロック E = バスサイクルの周波数で、`-clock` も E クロック）。シミュレーション時間（サイクル数 ÷ クロック）をホストの `steady_clock` に 1 ms 刻みで同期させるので、**ホストが十分速ければ PC の性能に依存せず指定クロックで動作**します（この環境（クラウドコンテナ）では約 10 MHz 相当まで出せます）。ホストが遅い場合は追いつけず遅れます（`-stats` に遅れたスライス数を表示）。テストでは `-turbo`（`-clock 0`）で制限なしに実行します。タイマ周期は `-tick`（サイクル数、既定 1,000,000 = 2 MHz で 2 Hz）か、`-tickhz F`（シミュレーション時間での周波数。周期 = `-clock` ÷ F。`-turbo` のときは 2 MHz を基準に換算）で別指定です。例: `-tickhz 50` = 40000 サイクル。ただし下記のとおり、50 Hz ではこの OS-9 ゲストの出力が化けます。

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
