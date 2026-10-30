# LYNX Traditional Chinese (zh-Hant) glossary

Reference for `resources/i18n/zh-Hant.txt` (language header "繁體中文", Taiwan + Hong Kong + Macau). English is the
source language. The finished Simplified Chinese table (`zh-Hans.txt`, see `zh-Hans-glossary.md`) was the base, but
the table is **not** a character conversion: vocabulary follows Taiwan software and DAW usage (Microsoft zh-TW UI
terms, Taiwanese producer forums and tutorials). The last column says where the choice differs from zh-Hans.

## Register and style

- **Register: neutral, as in Taiwanese software UIs.** Labels, buttons and menu items are short verb-object or noun
  phrases (「儲存專案」「重新命名通道」「刪除」); tooltips and messages are complete sentences. The subject "you" is
  normally omitted; where needed, **你** is used (Microsoft zh-TW style), never 您. Same as zh-Hans.
- **Concise:** prefer the shorter of two natural options; no parentheses the English does not have.
- **Characters:** Taiwan standard forms (OpenCC "tw" variants): 裡, 著, 線, 為, 群組…
- **Spacing:** no spaces between Chinese characters, nor between Chinese and Latin words / numbers
  (「開啟VST3外掛」「第{0}軌」). A half-width space is kept where the English has layout spacing (multiple spaces,
  " / ", " · ", "Label: value" groups) and between a number and its unit (10 ms, 440 Hz, -6 dB).
- **Punctuation:** full-width ，。：；？！（）、 in Chinese text. Quotes are **「」** (nested 『』) — Taiwan/HK
  convention; zh-Hans uses “”. ASCII parentheses may stay around purely Latin/numeric content ("(CC {0})").
  Position formats ("bar : beat : step") keep ASCII colons. A trailing "..." on menu items stays ASCII as in English.
- **ALL CAPS / Title Case** become normal words (FREQ → 頻率, MASTER → 主控). Cryptic FL knob abbreviations (Att, Dec,
  Rel, Res, Sat, Thres, Sprd, Ofs, ER, HQ…) and preset/algorithm names stay in English.
- **Counters:** 「{0}小節」「{0}個音符」「{0}個八度」; no plural forms.
- **On/off:** state labels 「開」/「關」; toggles 「啟用…」/「停用…」.

## Keys, mouse, shortcuts

- Mouse tokens `[LMB]` `[2xLMB]` `[RMB]` `[MMB]` `[Wheel]` stay exactly in English (drawn as glyphs).
- Key caps stay ASCII English: [Ctrl] [Shift] [Alt] [Space] [Enter] [Esc] [Del] [Tab] [Backspace], F1–F12.
  "Space" alone as a key name → 「空白鍵」 (Taiwan Windows; zh-Hans 空格). "Arrows" → 「方向鍵」.
- "Ctrl+drag" → 「Ctrl+拖曳」, "Alt-click" → 「Alt+點擊」, "click" → 「點擊」, "right-click" → 「按右鍵」/「右鍵」,
  "double-click" → 「雙擊」, "wheel" → 「滾輪」, "drag" → 「拖曳」 (zh-Hans 拖动 / 单击).

## Stays in English (unchanged)

Plugin, engine and product names (Sytrus, Harmor, Fruity …, 3x Osc, LYNX Tune, Facet EQ, VFX, FL Studio, VST/VST3/CLAP,
ASIO, WASAPI, ALSA, JACK, LYNX, Layer, Patcher), file extensions, units (Hz, kHz, dB, ms, %, BPM, st, ct), note names
(C4, F#), MIDI terms (MIDI, CC, NRPN, MPE), LFO, EQ, ADSR, LED, Q, tick, Mod X / Mod Y, SoundFont, CPU, preset and
algorithm names in "|" lists.

## Terms

| English | 繁體中文 | note (≠ zh-Hans) |
|---|---|---|
| Channel Rack | 通道機架 | window name; same as zh-Hans |
| Piano Roll | 鋼琴捲軸 | window name; zh-Hans 钢琴卷帘 (捲軸 is the Taiwanese/HK DAW term) |
| Playlist | 播放清單 | window name; zh-Hans 播放列表 (Taiwan: 清單) |
| Mixer | 混音器 | window name; zh-Hans 混音台 |
| Browser | 瀏覽器 | window name |
| project | 專案 | zh-Hans 工程 |
| pattern | 樣式 | Pattern/Song mode = 樣式模式 / 歌曲模式 |
| song (mode) | 歌曲 | |
| arrangement | 編排 | |
| clip | 片段 | audio clip = 音訊片段, automation clip = 自動化片段 |
| track (Playlist) | 軌道 | "Track {0}" = 軌道{0} |
| Mixer track / insert | 混音軌道 / 插入軌 | |
| channel | 通道 | audio channel (L/R) = 聲道 |
| send | 傳送 | zh-Hans 发送 (Taiwan: 傳送, also for "send a report") |
| slot | 插槽 | |
| Master | 主控 | |
| bar / beat / step | 小節 / 拍 / 步 | step sequencer = 步進音序器 |
| note | 音符 | remark = 備註 |
| velocity | 力度 | release velocity = 釋放力度 |
| pan | 聲像 | |
| volume / gain | 音量 / 增益 | |
| mute / solo | 靜音 / 獨奏 | |
| preset | 預設集 | zh-Hans 预设; Taiwan 預設 means "default", so presets are 預設集 (Adobe/Microsoft zh-TW) |
| default | 預設 | zh-Hans 默认 |
| sample (audio file) | 取樣 | zh-Hans 采样; sampler = 取樣器, sample rate = 取樣率 |
| synth | 合成器 | |
| plugin | 外掛 | zh-Hans 插件; VST外掛 is the Taiwanese term |
| effect | 效果器 / 效果 | |
| instrument | 樂器 | |
| automation | 自動化 | |
| quantize | 量化 | |
| snap | 貼齊 | zh-Hans 吸附 (Taiwan Microsoft "貼齊格線") |
| grid | 網格 | |
| loop | 循環 | |
| render | 渲染 | |
| export / import | 匯出 / 匯入 | zh-Hans 导出 / 导入 |
| stem | 分軌 | |
| tempo | 速度 | |
| time signature | 拍號 | |
| swing | 搖擺 | |
| pitch | 音高 | |
| envelope | 波封 | zh-Hans 包络 (波封 is the Taiwanese synth term) |
| filter / cutoff / resonance | 濾波器 / 截止頻率（截止）/ 共振 | |
| attack / hold / decay / sustain / release | 起音 / 保持 / 衰減 / 延音 / 釋放 | release: zh-Hans 释音 |
| wet / dry / mix | 濕 / 乾 / 混合 | |
| latency | 延遲 | |
| delay (effect, time) | 延時 | distinct from latency 延遲 |
| buffer | 緩衝區 | |
| audio | 音訊 | zh-Hans 音频 |
| audio device / sound card | 音訊裝置 / 音效卡 | zh-Hans 音频设备 / 声卡 |
| driver | 驅動程式 | "drive" (distortion) = 驅動 |
| input / output | 輸入 / 輸出 | |
| arpeggiator / chord / scale / root | 琶音器 / 和弦 / 音階 / 根音 | |
| key (musical) | 調 / 調性 | |
| key (keyboard) / shortcut | 鍵 / 按鍵 / 快速鍵 | zh-Hans 快捷键 |
| PC keyboard | 電腦鍵盤 | |
| octave / triplet | 八度 / 三連音 | |
| marker / metronome | 標記 / 節拍器 | |
| ghost notes | 幽靈音符 | |
| portamento / legato / slide | 滑奏 / 連奏 / 滑音 | |
| fade-in / fade-out / crossfade | 淡入 / 淡出 / 交叉淡化 | |
| normalize | 正規化 | zh-Hans 标准化 |
| reverse / invert phase | 倒放 / 反相 | |
| slice / stretch / freeze | 切片 / 拉伸 / 凍結 | |
| sidechain | 側鏈 | |
| compressor / limiter / expander | 壓縮器 / 限制器 / 擴展器 | |
| gate (dynamics) / noise gate | 閘門 / 噪音閘門 | zh-Hans 门 / 噪声门 |
| gate (arp / note length) | 門限 | same as zh-Hans |
| threshold / ratio / knee / lookahead | 閾值 / 壓縮比 / 拐點 / 前視 | |
| reverb | 殘響 | zh-Hans 混响 (殘響 is the Taiwanese term) |
| chorus / flanger / phaser / distortion | 合唱 / 鑲邊 / 移相 / 失真 | |
| EQ / equalizer | EQ / 等化器 | zh-Hans 均衡器 |
| shelf (EQ) | 層架 | zh-Hans 搁架 |
| tube | 真空管 | zh-Hans 电子管 |
| modulation / modulator | 調變 / 調變器 | zh-Hans 调制 |
| oscillator | 振盪器 | |
| noise | 噪音 | zh-Hans 噪声 |
| knob / fader / slider | 旋鈕 / 推桿 / 滑桿 | fader: zh-Hans 推子 |
| meter / peak | 電平表 / 峰值 | |
| stereo / mono | 立體聲 / 單聲道 | |
| parameter | 參數 | |
| undo / redo | 復原 / 重做 | zh-Hans 撤销 |
| new / open / save / save as | 新增 / 開啟 / 儲存 / 另存新檔 | zh-Hans 新建 / 打开 / 保存 / 另存为 |
| exit | 結束 | zh-Hans 退出 ("leave a mode" = 離開) |
| settings / options | 設定 / 選項 | zh-Hans 设置 |
| general (settings) | 一般 | zh-Hans 常规 |
| OK / Cancel / Apply | 確定 / 取消 / 套用 | Apply: zh-Hans 应用 |
| revert / reset | 還原 / 重設 | reset: zh-Hans 重置 |
| delete / remove / clear | 刪除 / 移除 / 清除 | |
| rename | 重新命名 | |
| clone / duplicate / copy | 再製 / 複製 / 複製 | clone: zh-Hans 克隆 |
| cut / paste | 剪下 / 貼上 | |
| select / select all | 選取（選擇）/ 全選 | |
| zoom in / out | 放大 / 縮小 | |
| enable / disable | 啟用 / 停用 | disable: zh-Hans 禁用 |
| show / hide | 顯示 / 隱藏 | |
| file / folder / file name | 檔案 / 資料夾 / 檔名 | zh-Hans 文件 / 文件夹 |
| template | 範本 | zh-Hans 模板 |
| autosave | 自動儲存 | |
| scan | 掃描 | |
| record / recording | 錄製 / 錄音 | |
| arm / record safe | 錄音預備 / 錄音保護 | |
| play / stop / pause | 播放 / 停止 / 暫停 | |
| help / about | 說明 / 關於LYNX | Help: zh-Hans 帮助 |
| window / menu / dialog | 視窗 / 選單 / 對話方塊 | zh-Hans 窗口 / 菜单 / 对话框 |
| toolbar / status bar / hint bar | 工具列 / 狀態列 / 提示列 | zh-Hans …栏 |
| lane | 欄 | never 通道 |
| mouse / cursor / wheel | 滑鼠 / 游標 / 滾輪 | zh-Hans 鼠标 / 光标 |
| program / process / thread | 程式 / 處理程序 / 執行緒 | zh-Hans 程序 / 进程 / 线程 |
| run (a program) | 執行 | zh-Hans 运行 |
| support | 支援 | zh-Hans 支持 |
| information / message | 資訊 / 訊息 | zh-Hans 信息 / 消息 |
| device | 裝置 | zh-Hans 设备 |
| screen / full screen | 螢幕 / 全螢幕 | zh-Hans 屏幕 / 全屏 |
| video / graphics card | 影片 / 顯示卡 | |
| memory / disk | 記憶體 / 磁碟 | |
| network | 網路 | |
| software / hardware / package | 軟體 / 硬體 / 套件 | |
| user / interface / font / icon | 使用者 / 介面 / 字型 / 圖示 | |
| search / refresh | 搜尋 / 重新整理 | |
| data / text / character | 資料 / 文字 / 字元 | |
| load / unload / install / uninstall | 載入 / 卸載 / 安裝 / 解除安裝 | |
| clipboard / thumbnail / wildcard | 剪貼簿 / 縮圖 / 萬用字元 | |
| custom / example / built-in | 自訂 / 範例 / 內建 | zh-Hans 自定义 / 示例 / 内置 |
| current | 目前 | zh-Hans 当前 |
| create / add | 建立 / 新增 | zh-Hans 创建 / 添加 |
| quality | 品質 | zh-Hans 质量 |
| real-time | 即時 | zh-Hans 实时 |
| priority | 優先順序 | |
| crash / freeze (app hang) | 崩潰 / 停止回應 | hang: zh-Hans 卡死 |
| generator | 產生器 | zh-Hans 发生器 |
| bank / patch | 音色庫 / 音色 | |
| link (MIDI / controller) / link (stereo, knobs) | 關聯 / 連結 | |
| bypass / ducking / formant | 旁通 / 閃避 / 共振峰 | |
| count-in / overdub | 預備拍 / 疊錄 | |
| dither | 抖動 | |
| tools: picker / pencil / brush / eraser / mute / knife / lasso / zoom / playback / slip | 拾取 / 鉛筆 / 畫筆 / 橡皮擦 / 靜音 / 刀片 / 套索 / 縮放 / 試聽 / 滑移 | |
| Kick / Snare / Hat / Clap | 大鼓 / 小鼓 / 腳踏鈸 / 拍手 | zh-Hans 底鼓 / 军鼓 / 踩镲 |
| make unique | 設為唯一 | |
| child (Layer) | 子通道 | |
| bit / bit depth / bit rate | 位元 / 位元深度 / 位元率 | |
| port | 連接埠 | |
| feedback (delay, reverb) | 回授 | zh-Hans 反馈 (回授 is the Taiwanese audio term) |
| analog / digital | 類比 / 數位 | zh-Hans 模拟 / 数字 |
| group (channels, tracks) / filter group | 群組 / 篩選群組 | zh-Hans 组 / 过滤组; "to group" = 編組 |
| match (EQ match, level matching) | 匹配 | search "words must match" = 符合 |
| vibrato / tremolo | 抖音 / 顫音 | |
| transistor | 電晶體 | zh-Hans 晶体管 |
| bus | 匯流排 | zh-Hans 总线 |
| algorithm / optimized | 演算法 / 最佳化 | zh-Hans 算法 / 优化 |
| performance (CPU) | 效能 | zh-Hans 性能 |
| favourites | 我的最愛 | zh-Hans 收藏 |
| in the background | 在背景 | zh-Hans 后台 |
| setup wizard | 設定精靈 | zh-Hans 设置向导 |
| Explorer / recycle bin | 檔案總管 / 資源回收筒 | Windows zh-TW names |
| file extension | 副檔名 | zh-Hans 扩展名 |
| function / operator (formulas) | 函式 / 運算子 | zh-Hans 函数 / 运算符 |
| touch (keyboard) | 觸控 | zh-Hans 触摸 |
| contrast | 對比度 | A/B compare = A/B比較 |
| remote / vector / gradient | 遠端 / 向量 / 漸層 | |
| capture | 擷取 | zh-Hans 捕获 |
| cable (Patcher) / cable (physical) | 連線 / 纜線 | |
| return (hardware insert) | 回送 | |
| via / through (a tool) | 透過 | "passes through unchanged" stays 原樣通過 |
