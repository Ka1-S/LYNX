# LYNX Simplified Chinese (zh-Hans) glossary

Reference for `resources/i18n/zh-Hans.txt` (language header "简体中文", mainland China). English is the source
language; the pt-BR table was only used as a hint for meaning. This table is also the base for zh-Hant
(Traditional Chinese): the terms below are chosen so that they convert cleanly (钢琴卷帘 → 鋼琴捲簾, 混音台 → 混音台,
工程 → 工程); Taiwan-specific vocabulary differences are noted in the last column where they matter.

## Register and style

- **Register: neutral, as in Chinese DAW software (FL Studio 中文版, Cubase/Studio One 简体中文).** Labels, buttons,
  menu items and column headers are short verb-object or noun phrases: 「保存工程」「重命名通道」「删除」. Help text,
  tooltips and dialog messages are complete sentences. The subject "you" is normally omitted; where it cannot be
  omitted, **你** is used (Microsoft zh-CN style), never 您.
- **Concise.** Prefer the shorter of two natural options; no parenthetical explanations the English does not have.
- **Spacing rule: no spaces between Chinese characters, and no space between Chinese and Latin words or numbers**
  (「打开VST3插件」「第{0}轨」「{0}个音符」). A half-width space is kept only where the English has layout spacing
  (multiple spaces, around " / ", " · ", " - ", between "Label: value" groups) and between a number and its unit
  (10 ms, 440 Hz, -6 dB), as in the English.
- **Punctuation:** full-width ，。：；？！（） in Chinese text; “” for quoted names (‘’ nested), so \" is rarely needed.
  "Label: {0}" → 「标签：{0}」 (full-width colon after Chinese). Purely Latin/numeric parentheses such as "(x{1})",
  "(CC {0})" may stay ASCII. Position formats like "bar : beat : step" keep ASCII colons. Ellipsis "..." at the end
  of menu items stays exactly as in English (ASCII). Lists use 、 between Chinese items and ， between clauses.
- **Title Case / ALL CAPS** have no equivalent: ALL-CAPS panel labels (FREQ, GAIN, MASTER) are translated as normal
  words (频率, 增益, 主控). Cryptic knob abbreviations from FL plugins (Att, Dec, Rel, Res, Sat, Thres, Sprd, Ofs, ER,
  HQ…) and preset/algorithm names stay in English.
- **Counters:** "{0} bars" → 「{0}小节」, "{0} notes" → 「{0}个音符」, "{0} octaves" → 「{0}个八度」; Chinese has no
  plural, so singular/plural strings get the same form ("1 bar" → 「1小节」).
- **On/off:** state labels 「开」/「关」 (「LFO：开」); toggles 「启用…」/「禁用…」 or 「…开/关」.

## Keys, mouse, shortcuts

- Mouse tokens `[LMB]` `[2xLMB]` `[RMB]` `[MMB]` `[Wheel]` stay exactly in English (drawn as glyphs).
- Key caps stay ASCII English everywhere, as printed on Chinese keyboards: [Ctrl] [Shift] [Alt] [Space] [Enter]
  [Esc] [Del] [Tab] [Backspace], F1–F12. "Space" alone as a key name → 「空格」 (as Chinese Windows names it in
  shortcut lists); inside "[Space]" it stays ASCII. "Arrows" → 「方向键」.
- "Ctrl+drag" → 「Ctrl+拖动」, "Alt-click" → 「Alt+单击」, "click" → 「单击」 (「点击」 in sentences is also fine),
  "right-click" → 「右键单击」 / 「右键」 in short hints, "double-click" → 「双击」, "wheel" → 「滚轮」, "drag" → 「拖动」.

## Stays in English (unchanged)

Plugin, engine and product names (Sytrus, Harmor, Fruity …, 3x Osc, LYNX Tune, Facet EQ, VFX, FL Studio, VST/VST3/CLAP,
ASIO, WASAPI, ALSA, JACK, LYNX, Layer, Patcher as plugin names), file extensions, units (Hz, kHz, dB, ms, %, BPM,
st, ct), note names (C4, F#), MIDI terms (MIDI, CC, NRPN, MPE), LFO, EQ, ADSR, LED, Q, tick, Mod X / Mod Y,
SoundFont, CPU, preset/algorithm names in "|" lists.

## Terms

| English | 简体中文 | note |
|---|---|---|
| Channel Rack | 通道机架 | window name (FL Studio 中文版) |
| Piano Roll | 钢琴卷帘 | window name; zh-Hant 鋼琴捲簾 |
| Playlist | 播放列表 | window name |
| Mixer | 混音台 | window name |
| Browser | 浏览器 | window name; zh-Hant 瀏覽器 |
| project | 工程 | always 工程 (not 项目): 新建工程, 保存工程 |
| pattern | 样式 | FL 中文 term; Pattern/Song mode = 样式模式 / 歌曲模式 |
| song (mode) | 歌曲 | |
| arrangement | 编排 | |
| clip | 片段 | audio clip = 音频片段, automation clip = 自动化片段, pattern clip = 样式片段 |
| track (Playlist) | 轨道 | "Track {0}" = 轨道{0} |
| Mixer track / insert | 混音轨道 / 插入轨 | |
| channel | 通道 | |
| send | 发送 | |
| slot | 插槽 | effect slot = 效果插槽 |
| Master | 主控 | Master track = 主控轨道 |
| bar | 小节 | |
| beat | 拍 | |
| step | 步 | step sequencer = 步进音序器 |
| tick | tick | kept |
| note | 音符 | "note" as a remark = 备注 |
| velocity | 力度 | release velocity = 释放力度 |
| pan | 声像 | |
| volume | 音量 | |
| gain | 增益 | |
| mute / unmute | 静音 / 取消静音 | |
| solo | 独奏 | |
| preset | 预设 | |
| sample (audio file) | 采样 | sampler = 采样器; sample rate = 采样率 |
| synth | 合成器 | |
| plugin | 插件 | |
| effect | 效果器 / 效果 | effect plugin = 效果器; "Effects: {0}" = 效果：{0} |
| instrument | 乐器 | |
| automation | 自动化 | |
| quantize | 量化 | |
| snap | 吸附 | |
| grid | 网格 | |
| loop | 循环 | loop (audio file) = loop 采样 → 循环采样 |
| render | 渲染 | |
| export / import | 导出 / 导入 | |
| stem | 分轨 | stems = 分轨 |
| tempo | 速度 | BPM stays; tempo automation = 速度自动化 |
| time signature | 拍号 | |
| swing | 摇摆 | |
| pitch | 音高 | fine pitch = 微调音高 |
| envelope | 包络 | |
| LFO | LFO | |
| filter | 滤波器 | |
| cutoff | 截止频率 / 截止 | 截止 in narrow labels |
| resonance | 共振 | |
| attack / hold / decay / sustain / release | 起音 / 保持 / 衰减 / 延音 / 释音 | |
| wet / dry | 湿 / 干 | |
| mix (knob) | 混合 | |
| latency | 延迟 | |
| delay (effect, time) | 延时 | distinct from latency 延迟 |
| buffer | 缓冲区 | buffer size = 缓冲区大小 |
| audio device | 音频设备 | sound card = 声卡 |
| input / output | 输入 / 输出 | |
| MIDI controller | MIDI控制器 | |
| arpeggiator | 琶音器 | |
| chord | 和弦 | |
| scale | 音阶 | |
| root | 根音 | |
| key (musical) | 调 / 调性 | |
| key (keyboard) | 键 / 按键 | keyboard shortcut = 快捷键; PC keyboard = 电脑键盘 |
| octave | 八度 | |
| triplet | 三连音 | 1/8 triplet = 1/8三连音 |
| major / minor | 大调 / 小调 | modes: 多利亚, 弗里几亚, 利底亚, 混合利底亚, 洛克里亚; pentatonic = 五声音阶 |
| marker | 标记 | |
| metronome | 节拍器 | |
| tools: picker / pencil / brush / eraser / mute / knife / lasso / zoom / playback / slip | 拾取 / 铅笔 / 画笔 / 橡皮擦 / 静音 / 刀片 / 套索 / 缩放 / 试听 / 滑移 | |
| ghost notes | 幽灵音符 | |
| slide (808) | 滑音 | |
| portamento | 滑奏 | "Porta" label = 滑奏 |
| legato | 连奏 | |
| fade-in / fade-out | 淡入 / 淡出 | crossfade = 交叉淡化 |
| normalize | 标准化 | |
| reverse (sample) | 倒放 | |
| invert phase | 反相 | phase = 相位 |
| slice / slicer | 切片 / 切片器 | |
| stretch | 拉伸 | time stretch = 时间拉伸 |
| freeze | 冻结 | |
| sidechain | 侧链 | |
| compressor / limiter / gate | 压缩器 / 限制器 / 门 | noise gate = 噪声门; threshold = 阈值; ratio = 压缩比 |
| reverb / delay / chorus / flanger / phaser / distortion | 混响 / 延时 / 合唱 / 镶边 / 移相 / 失真 | |
| EQ (parametric) | EQ / 参数均衡器 | band = 频段; shelf = 搁架; low/high cut = 低切/高切; band-pass = 带通; notch = 陷波; peak = 峰值 |
| oscillator | 振荡器 | |
| waveform: sine / triangle / saw / square / pulse / noise | 正弦波 / 三角波 / 锯齿波 / 方波 / 脉冲波 / 噪声 | |
| knob / fader | 旋钮 / 推子 | |
| meter | 电平表 | |
| peak (level) | 峰值 | |
| stereo / mono | 立体声 / 单声道 | stereo separation = 立体声分离 |
| parameter | 参数 | |
| undo / redo | 撤销 / 重做 | |
| new / open / save / save as | 新建 / 打开 / 保存 / 另存为 | |
| open recent | 最近打开 | |
| exit | 退出 | |
| settings / options | 设置 / 选项 | |
| OK / Cancel / Apply | 确定 / 取消 / 应用 | |
| Revert | 还原 | |
| delete / remove | 删除 / 移除 | 移除 when not destructive ("remove from list") |
| clear | 清除 | |
| rename | 重命名 | |
| clone / duplicate | 克隆 / 复制 | |
| copy / cut / paste | 复制 / 剪切 / 粘贴 | |
| select / select all | 选择 / 全选 | |
| zoom in / out | 放大 / 缩小 | |
| default | 默认 | |
| reset | 重置 | |
| enable / disable | 启用 / 禁用 | on / off (state) = 开 / 关 |
| show / hide | 显示 / 隐藏 | |
| color | 颜色 | |
| length | 长度 | |
| position | 位置 | |
| file / folder | 文件 / 文件夹 | |
| template | 模板 | |
| autosave | 自动保存 | |
| scan (plugins) | 扫描 | |
| record / recording | 录制 / 录音 | 录音 for audio recordings (录音文件夹) |
| arm (record) / record safe | 录音预备 / 录音保护 | disarm = 取消录音预备 |
| play / stop / pause | 播放 / 停止 / 暂停 | |
| help / about | 帮助 / 关于LYNX | |
| language | 语言 | |
| none | 无 | |
| random | 随机 | |
| Kick / Snare / Hat / Clap | 底鼓 / 军鼓 / 踩镲 / 拍手 | |
| tooltip / hint bar | 工具提示 / 提示栏 | |
| make unique | 设为唯一 | |
| last tweaked | 最后调整的 | |
| child (Layer) | 子通道 | |
| round trip (latency) | 往返 | |
| de-esser | 齿音消除器 | |
| early reflections / tail | 早期反射 / 尾音 | |
| bridged (plugin) | 桥接 | |
| CPU | CPU | |
| lane (graph / event lane) | 栏 | never 通道 (that is "channel"): 图形栏, 底部栏 |
| generator | 发生器 | |
| bank (presets/SoundFont) | 音色库 | patch = 音色 |
| link (MIDI / controller) | 关联 | link (stereo, knobs) = 链接 |
| bypass | 旁通 | |
| ghost (tool/notes) | 幽灵音符 | |
| stretch handle / warp marker | 拉伸手柄 / 变速标记 | |
| count-in | 预备拍 | |
| overdub | 叠录 | |
| punch in / out | 插入录音 / 插出 | |
| dither / noise shaping | 抖动 / 噪声整形 | |
| lookahead | 前视 | |
| knee | 拐点 | |
| ratio | 压缩比 | |
| ducking | 闪避 | |
| formant | 共振峰 | |
| voicing (character mode) | 音色风格 | |
| Patcher / Layer (plugins) | Patcher / Layer | kept; Layer children = 子通道 |
| "round NNN" (dev versions in a few strings) | 第NNN轮 | |
| Name lists of algorithms/amp models/presets ("Clean|Tape|…", "Glass valve") | kept in English | functional lists (Off/On, Low/Mid/High, waveforms) are translated |
