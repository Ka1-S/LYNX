# LYNX Japanese (ja) glossary

Reference for `resources/i18n/ja.txt` (language header "日本語"). English is the source language;
the pt-BR table was only used as a hint for meaning.

## Register and style

- **Register: neutral-polite, as in Japanese DAW software (FL Studio JP, Cubase JP, Studio One JP).**
  Labels, buttons, menu items and column headers are short noun phrases or verb-noun phrases without
  です/ます: 「プロジェクトを保存」「チャンネル名を変更」「削除」. Full-sentence tooltips, help text, dialog
  messages and status messages use です/ます (「保存されていない変更があります。」). No direct "you"
  (あなた) — Japanese UI omits the subject.
- **Concise.** Prefer katakana loanwords that Japanese producers actually use (プリセット, サンプル, ループ,
  センド, スウィング…) and the shorter of two natural options. No parenthetical explanations the English
  does not have.
- **No spaces between Japanese words.** A half-width space is kept only where the English has layout
  spacing (multiple spaces, around " / ", " · ", before/after placeholders when the English has them next to
  Latin text) and between Latin words/numbers and units (10 ms, 440 Hz, -6 dB).
- **Punctuation:** full-width 、。 in Japanese sentences; 「」 for quoted names (『』 nested), so \" is
  rarely needed. Full-width ？！ after Japanese text. Colons in "Label: {0}" stay ASCII ": " (UI tradition,
  keeps placeholders aligned); parentheses in Japanese text use full-width （） only inside full sentences —
  short labels keep ASCII ( ) for width. Ellipsis "..." is kept exactly as in English (ASCII).
- **Title Case / ALL CAPS** have no equivalent: ALL-CAPS panel labels (FREQ, GAIN, SNAP, MASTER) are translated like
  normal words (周波数, ゲイン, スナップ, マスター). Cryptic knob abbreviations from FL plugins (Att, Dec, Rel, Res, Sat,
  Thres, Sprd, Ofs, ER, HQ...) and preset/algorithm names stay in English.
- **On/off toggles:** 「録音のオン/オフ」 (noun + の + オン/オフ), state labels 「LFO: オン」.
- **Counters:** "{0} bars" → 「{0}小節」, "{0} notes" → 「{0}個のノート」 / 「ノート{0}個」, "{0} octaves" →
  「{0}オクターブ」; Japanese has no plural, so singular/plural strings get the same form.

## Keys, mouse, shortcuts

- Mouse tokens `[LMB]` `[2xLMB]` `[RMB]` `[MMB]` `[Wheel]` stay exactly in English (drawn as glyphs).
- Key caps stay ASCII English everywhere, as printed on Japanese keyboards: [Ctrl] [Shift] [Alt] [Space]
  [Enter] [Esc] [Del] [Tab] [Backspace], F1–F12. "Space" alone as a key name → 「Space」; "Arrows" → 「矢印キー」.
- "Ctrl+drag" → 「Ctrl+ドラッグ」, "Alt-click" → 「Alt+クリック」, "right-click" → 「右クリック」,
  "double-click" → 「ダブルクリック」, "wheel" → 「ホイール」, "drag" → 「ドラッグ」.

## Stays in English (unchanged)

Plugin, engine and product names (Sytrus, Harmor, Fruity …, 3x Osc, LYNX Tune, Facet EQ, VFX, FL Studio,
VST/VST3/CLAP, ASIO, WASAPI, ALSA, JACK, LYNX), file extensions, units (Hz, kHz, dB, ms, %, BPM, st, ct), note
names (C4, F#), MIDI terms (MIDI, CC, NRPN, MPE), LFO, EQ, LED, Q, preset/algorithm names in "|" lists
(Concert Hall, Plate…, amp/tape model names such as Glass valve, Stack 68). The "Pattern" / "Song" transport
labels are translated as パターン / ソング.

## Terms

| English | 日本語 | note |
|---|---|---|
| Channel Rack | チャンネルラック | window name (FL Studio JP) |
| Piano Roll | ピアノロール | window name |
| Playlist | プレイリスト | window name |
| Mixer | ミキサー | window name |
| Browser | ブラウザー | window name (long-vowel form, Microsoft/FL JP style) |
| pattern | パターン | |
| song (mode) | ソング | Pattern/Song mode = パターンモード / ソングモード |
| arrangement | アレンジメント | |
| clip | クリップ | audio clip = オーディオクリップ, automation clip = オートメーションクリップ |
| track (Playlist/Mixer) | トラック | Mixer insert track = ミキサートラック |
| channel | チャンネル | |
| insert | インサート | |
| send | センド | |
| slot | スロット | |
| Master | マスター | |
| bar | 小節 | |
| beat | 拍 | |
| step | ステップ | |
| tick | ティック | |
| note | ノート | musical note in an editor; "note" as remark = 注記 |
| velocity | ベロシティ | release velocity = リリースベロシティ |
| pan | パン | |
| volume | ボリューム | |
| gain | ゲイン | |
| mute / unmute | ミュート / ミュート解除 | |
| solo | ソロ | |
| preset | プリセット | |
| sample | サンプル | |
| sampler | サンプラー | |
| synth | シンセ | |
| plugin | プラグイン | |
| effect | エフェクト | |
| instrument | インストゥルメント | 楽器 where space is tight (plugin type column, plugin counts) |
| automation | オートメーション | |
| quantize | クオンタイズ | |
| snap | スナップ | |
| grid | グリッド | |
| loop | ループ | |
| render | レンダリング | |
| export / import | エクスポート / インポート | |
| stem | ステム | |
| tempo | テンポ | BPM stays |
| time signature | 拍子 | |
| swing | スウィング | |
| pitch | ピッチ | fine pitch = ファインピッチ |
| envelope | エンベロープ | |
| LFO | LFO | |
| filter | フィルター | |
| cutoff | カットオフ | |
| resonance | レゾナンス | |
| attack / hold / decay / sustain / release | アタック / ホールド / ディケイ / サステイン / リリース | |
| wet / dry | ウェット / ドライ | |
| mix (knob) | ミックス | |
| latency | レイテンシー | |
| buffer | バッファー | |
| sample rate | サンプルレート | |
| audio device | オーディオデバイス | |
| input / output | 入力 / 出力 | |
| MIDI controller | MIDIコントローラー | |
| arpeggiator | アルペジエーター | |
| chord | コード | |
| scale | スケール | |
| root | ルート | |
| key (musical) | キー | |
| key (keyboard) | キー | keyboard shortcut = ショートカット |
| octave | オクターブ | |
| triplet | 3連符 | 1/8 triplet = 1/8 3連 |
| major / minor | メジャー / マイナー | modes: ドリアン, フリジアン, リディアン… |
| marker | マーカー | |
| metronome | メトロノーム | |
| tool: pencil / brush / eraser / knife / lasso / slip | ペンシル / ブラシ / 消しゴム / ナイフ / 投げ縄 / スリップ | picker = ピッカー |
| ghost notes | ゴーストノート | |
| slide (808) | スライド | |
| portamento | ポルタメント | |
| legato | レガート | |
| fade-in / fade-out | フェードイン / フェードアウト | |
| normalize | ノーマライズ | |
| reverse | リバース | |
| slice / slicer | スライス / スライサー | |
| stretch | ストレッチ | |
| freeze | フリーズ | |
| sidechain | サイドチェーン | |
| compressor / limiter / reverb / delay / chorus / flanger / phaser / distortion | コンプレッサー / リミッター / リバーブ / ディレイ / コーラス / フランジャー / フェイザー / ディストーション | |
| EQ (parametric) | EQ (パラメトリックEQ) | band = バンド, shelf = シェルフ, low/high cut = ローカット/ハイカット |
| oscillator | オシレーター | |
| waveform: sine / triangle / saw / square / pulse / noise | サイン / トライアングル / ノコギリ / スクエア / パルス / ノイズ | |
| knob / fader | ノブ / フェーダー | |
| meter | メーター | |
| peak | ピーク | |
| stereo / mono | ステレオ / モノ | |
| phase | 位相 | invert phase = 位相を反転 |
| parameter | パラメーター | |
| undo / redo | 元に戻す / やり直し | |
| new / open / save / save as | 新規 / 開く / 保存 / 名前を付けて保存 | |
| open recent | 最近使ったファイル | |
| exit | 終了 | |
| settings / options | 設定 / オプション | |
| OK / Cancel / Apply | OK / キャンセル / 適用 | |
| Revert | 元に戻す | |
| delete / remove | 削除 | "remove from" = 〜から外す when it is not destructive |
| clear | クリア | |
| rename | 名前を変更 | |
| clone / duplicate | 複製 | |
| copy / cut / paste | コピー / カット / 貼り付け | |
| select / select all | 選択 / すべて選択 | |
| zoom in / out | ズームイン / ズームアウト | |
| default | デフォルト | |
| reset | リセット | |
| enable / disable | 有効 / 無効 | on / off (state) = オン / オフ |
| show / hide | 表示 / 非表示 | |
| color | カラー | |
| length | 長さ | |
| position | 位置 | |
| file / folder | ファイル / フォルダー | |
| project | プロジェクト | |
| template | テンプレート | |
| autosave | 自動保存 | |
| scan (plugins) | スキャン | |
| recording / record | 録音 | |
| play / stop / pause | 再生 / 停止 / 一時停止 | |
| help / about | ヘルプ / LYNXについて | |
| language | 言語 | |
| none | なし | |
| random | ランダム | |
| Kick / Snare / Hat / Clap | キック / スネア / ハット / クラップ | |
| tooltip / hint bar | ツールチップ / ヒントバー | |
| arm (record) / record safe | 録音待機 / 録音保護 | disarm = 録音待機を解除 |
| make unique | ユニーク化 | |
| last tweaked | 最後に操作 | |
| ghost / picker / Patcher / Layer | ゴースト / ピッカー / パッチャー / Layer | Layer = plugin name, kept |
| child (Layer) | 子 | |
| round trip (latency) | ラウンドトリップ | |
| de-esser / de-ploser | ディエッサー / ディプローサー | |
| early reflections / tail | 初期反射 / テール | |
| smart disable | スマート無効化 | |
| bridged | ブリッジ | |
| Mod X / Mod Y | Mod X / Mod Y | kept |
| SoundFont | SoundFont | kept |
| CPU | CPU | |
