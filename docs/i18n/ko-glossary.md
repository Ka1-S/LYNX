# LYNX Korean (ko) glossary

Reference for `resources/i18n/ko.txt` (language header "한국어"). English is the source language;
the pt-BR table was only used as a hint for meaning.

## Register and style

- **Register: polite 합니다/하세요 style for sentences, as in Korean DAW software (FL Studio KR community,
  Cubase/Studio One KR, Windows KR).** Labels, buttons, menu items and column headers are concise noun
  phrases or plain commands without an ending: "프로젝트 저장", "채널 이름 변경", "삭제". Full-sentence
  tooltips, help text, dialog messages and status messages use 합니다/하세요 ("저장하지 않은 변경 사항이
  있습니다.", "노브를 움직이세요."). Short tooltip fragments that describe what a control does may end in a
  noun phrase or "~합니다". No direct "you" (당신) — Korean UI omits the subject.
- **Concise.** Prefer loanwords Korean producers actually use (프리셋, 샘플, 루프, 센드, 스윙, 벨로시티…) and the
  shorter of two natural options. No parenthetical explanations the English does not have.
- **Normal Korean word spacing (띄어쓰기).** Units keep a space after numbers as in English (10 ms, 440 Hz).
- **Punctuation:** ASCII punctuation (. , : ? ! ( )) as in modern Korean UI. Quoted names use “ ” or ‘ ’
  (so \" is rarely needed); when the English uses \"…\" the translation may keep \"…\". Ellipsis "..." kept
  exactly as in English. Colons in "Label: {0}" stay ": ".
- **Title Case** has no equivalent. ALL-CAPS labels (SNAP, MASTER, FREQ, GAIN, CORRECTION…) are translated
  like normal labels (Hangul has no case): 스냅, 마스터, 주파수, 게인, 보정. Product-like caps names stay
  (FLEX-TUNE, CPU, RAM, GR, HQ).
- **Abbreviated knob labels** (Att, Rel, Res, Thres, Vol, Pn, Vl…) are written as the short Korean word
  (어택, 릴리스, 레조, 스레시, 볼륨, 팬, 볼륨); Hangul is wide, so prefer 2–3 syllables.
- **"Mod" (modulation) vs "Mode":** 모드 means *mode* only. Modulation abbreviations keep Latin "Mod"
  (Mod 주파수, 컷오프 Mod, Mod X / Mod Y); spelled out it is 모듈레이션 (Mod wheel = 모듈레이션 휠).
- **Preset / algorithm / model names** in "|" lists and as single strings (reverb algorithms, plate metals,
  echo machines, chorus modes, valve/reel/amp models such as "Glass valve", "Combo 59") stay in English.
  Descriptive style words get Hangul transliteration (클린, 펀치, 웜, 브라이트, 다크, 빈티지, 글루, 덴스).
- **Counters:** "{0} bars" → "{0}마디", "{0} notes" → "노트 {0}개", "{0} octaves" → "{0}옥타브",
  "{0} steps" → "{0}스텝". Korean has no plural, so singular/plural strings get the same form.
- **Particles after placeholders:** {0} may end in a vowel or a consonant, so 을/를, 이/가, 은/는, 와/과
  are avoided right after a placeholder; restructure ("{0}: 열 수 없음") or use particles that do not
  depend on the final consonant (에, 에서, 의, 도).

## Keys, mouse, shortcuts

- Mouse tokens `[LMB]` `[2xLMB]` `[RMB]` `[MMB]` `[Wheel]` stay exactly in English (drawn as glyphs).
- Key caps stay ASCII English everywhere, as printed on Korean keyboards: [Ctrl] [Shift] [Alt] [Space]
  [Enter] [Esc] [Del] [Tab] [Backspace], F1–F12. "Space" alone as a key name → "Space"; "Arrows" → "방향키".
- "Ctrl+drag" → "Ctrl+드래그", "Alt-click" → "Alt+클릭", "right-click" → "우클릭", "double-click" →
  "더블 클릭", "wheel" → "휠", "drag" → "드래그", "click" → "클릭".

## Stays in English (unchanged)

Plugin, engine and product names (Sytrus, Harmor, Fruity …, 3x Osc, LYNX Tune, Facet EQ, VFX, FL Studio,
VST/VST3/CLAP, ASIO, WASAPI, ALSA, JACK, LYNX), file extensions, units (Hz, kHz, dB, ms, %, BPM, st, ct), note
names (C4, F#), MIDI terms (MIDI, CC, NRPN, MPE), LFO, EQ, LED, Q, preset/algorithm names in "|" lists.

## Terms

| English | 한국어 | note |
|---|---|---|
| Channel Rack | 채널 랙 | window name (FL Studio KR community) |
| Piano Roll | 피아노 롤 | window name |
| Playlist | 플레이리스트 | window name |
| Mixer | 믹서 | window name |
| Browser | 브라우저 | window name |
| pattern | 패턴 | |
| song (mode) | 송 | Pattern/Song mode = 패턴 모드 / 송 모드 (FL KR usage) |
| arrangement | 어레인지먼트 | |
| clip | 클립 | audio clip = 오디오 클립, automation clip = 오토메이션 클립 |
| track | 트랙 | Mixer insert track = 믹서 트랙 |
| channel | 채널 | |
| insert | 인서트 | |
| send | 센드 | |
| slot | 슬롯 | |
| Master | 마스터 | |
| bar | 마디 | |
| beat | 박 | "Beat" grid label = 박 |
| step | 스텝 | |
| tick | 틱 | |
| note | 노트 | musical note; "note" as remark = 메모 |
| velocity | 벨로시티 | release velocity = 릴리스 벨로시티 |
| pan | 팬 | |
| volume | 볼륨 | |
| gain | 게인 | |
| mute / unmute | 뮤트 / 뮤트 해제 | |
| solo | 솔로 | |
| preset | 프리셋 | |
| sample | 샘플 | |
| sampler | 샘플러 | |
| synth | 신스 | |
| plugin | 플러그인 | |
| effect | 이펙트 | |
| instrument | 악기 | |
| automation | 오토메이션 | |
| quantize | 퀀타이즈 | |
| snap | 스냅 | |
| grid | 그리드 | |
| loop | 루프 | |
| render | 렌더링 | |
| export / import | 내보내기 / 가져오기 | |
| stem | 스템 | |
| tempo | 템포 | BPM stays |
| time signature | 박자 | |
| swing | 스윙 | |
| pitch | 피치 | fine pitch = 미세 피치 |
| envelope | 엔벨로프 | |
| LFO | LFO | |
| filter | 필터 | |
| cutoff | 컷오프 | |
| resonance | 레조넌스 | |
| attack / hold / decay / sustain / release | 어택 / 홀드 / 디케이 / 서스테인 / 릴리스 | |
| wet / dry | 웻 / 드라이 | |
| mix (knob) | 믹스 | |
| latency | 레이턴시 | |
| buffer | 버퍼 | |
| sample rate | 샘플 레이트 | |
| audio device | 오디오 장치 | |
| input / output | 입력 / 출력 | |
| MIDI controller | MIDI 컨트롤러 | |
| arpeggiator | 아르페지에이터 | |
| chord | 코드 | |
| scale | 스케일 | |
| root | 루트 | |
| key (musical) | 키 | |
| key (keyboard) | 키 | keyboard shortcut = 단축키 |
| octave | 옥타브 | |
| triplet | 셋잇단 | 1/8 triplet = 1/8 셋잇단 |
| major / minor | 메이저 / 마이너 | modes: 도리안, 프리지안, 리디안… |
| marker | 마커 | |
| metronome | 메트로놈 | |
| tool: pencil / brush / eraser / knife / lasso / slip | 연필 / 브러시 / 지우개 / 칼 / 올가미 / 슬립 | picker = 피커 |
| ghost notes | 고스트 노트 | |
| slide (808) | 슬라이드 | |
| portamento | 포르타멘토 | |
| legato | 레가토 | |
| fade-in / fade-out | 페이드 인 / 페이드 아웃 | |
| normalize | 노멀라이즈 | |
| reverse | 리버스 | |
| slice / slicer | 슬라이스 / 슬라이서 | |
| stretch | 스트레치 | |
| freeze | 프리즈 | |
| sidechain | 사이드체인 | |
| compressor / limiter / reverb / delay / chorus / flanger / phaser / distortion | 컴프레서 / 리미터 / 리버브 / 딜레이 / 코러스 / 플랜저 / 페이저 / 디스토션 | |
| EQ (parametric) | EQ (파라메트릭 EQ) | band = 밴드, shelf = 셸프, low/high cut = 로우 컷/하이 컷 |
| oscillator | 오실레이터 | |
| waveform: sine / triangle / saw / square / pulse / noise | 사인 / 삼각파 / 톱니파 / 사각파 / 펄스 / 노이즈 | |
| knob / fader | 노브 / 페이더 | |
| meter | 미터 | |
| peak | 피크 | |
| stereo / mono | 스테레오 / 모노 | |
| phase | 위상 | invert phase = 위상 반전 |
| parameter | 파라미터 | |
| undo / redo | 실행 취소 / 다시 실행 | |
| new / open / save / save as | 새로 만들기 / 열기 / 저장 / 다른 이름으로 저장 | |
| open recent | 최근 파일 열기 | |
| exit | 종료 | |
| settings / options | 설정 / 옵션 | |
| OK / Cancel / Apply | 확인 / 취소 / 적용 | |
| Revert | 되돌리기 | |
| delete / remove | 삭제 / 제거 | remove = 제거 when not destructive |
| clear | 지우기 | |
| rename | 이름 변경 | |
| clone / duplicate | 복제 | |
| copy / cut / paste | 복사 / 잘라내기 / 붙여넣기 | |
| select / select all | 선택 / 모두 선택 | |
| zoom in / out | 확대 / 축소 | tool name "Zoom" = 줌 |
| default | 기본값 | as adjective: 기본 |
| reset | 초기화 | |
| enable / disable | 사용 / 사용 안 함 | on / off (state) = 켜짐 / 꺼짐; "Off" as option = 끔 |
| show / hide | 표시 / 숨기기 | |
| color | 색상 | |
| length | 길이 | |
| position | 위치 | |
| file / folder | 파일 / 폴더 | |
| project | 프로젝트 | |
| template | 템플릿 | |
| autosave | 자동 저장 | |
| scan (plugins) | 스캔 | |
| record / recording | 녹음 | |
| play / stop / pause | 재생 / 정지 / 일시 정지 | |
| help / about | 도움말 / LYNX 정보 | |
| language | 언어 | |
| none | 없음 | |
| random | 랜덤 | |
| Kick / Snare / Hat / Clap | 킥 / 스네어 / 햇 / 클랩 | |
| tooltip / hint bar | 툴팁 / 힌트 바 | |
| marker / loop region | 마커 / 루프 구간 | |
| CPU | CPU | |
| generator | 제너레이터 | |
| take (recording) | 테이크 | |
| arm (record) / disarm | 녹음 대기 / 녹음 대기 해제 | record safe = 녹음 보호 |
| playhead | 재생 헤드 | |
| ruler | 눈금자 | |
| tap tempo | 탭 템포 | tapper = 태퍼 |
| ghost / child (Layer) | 고스트 / 자식 | |
| threshold / ratio / knee | 스레숄드 / 비율 / 니 | |
| lookahead | 룩어헤드 | |
| bypass | 바이패스 | |
| crossover | 크로스오버 | |
| round trip (latency) | 왕복 지연 | |
| wrapper / bridged | 래퍼 / 브리지 | |
| smart disable | 스마트 비활성화 | |
| Patcher, Surface, Map | Patcher, Surface, Map | LYNX/FL plugin component names, kept |
| SoundFont | SoundFont | |
