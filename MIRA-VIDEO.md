# MIRA-VIDEO.md — scoring to picture

**Opened 2026-09-19.** A plan, not a record: nothing here is built yet. When something
lands, mark its task and move the reasoning into the past tense — the same discipline
[MIRA-GENERATE.md](MIRA-GENERATE.md) follows.

Read [CANVAS.md](CANVAS.md) first: this is an extension of the canvas, not a new tool.
Read [ARCHITECTURE.md §6](ARCHITECTURE.md) for the audio paths it has to fit into.

---

## 1. What this is for

**Load a cut, see it, hear it, and put generated music against it.** A temp score: SA3
references laid against picture, exported as stems, finished in a DAW.

That is not a new application. The canvas already has a timeline, a playhead, tracks that
sum, and blocks that own their generator. A block sitting at 1:32 **is** a cue at 1:32. What
is missing is the picture, a clock a picture editor would recognise, and the film's own
audio to write against.

### What it is not

- Not a video editor. mira never re-encodes, never trims, never exports picture.
- Not a conform tool. No EDL, no AAF, no OMF.
- Not a mixing stage. Stems go to a DAW; that is the whole point of the export.

---

## 2. The four decisions, made

Taken before any code, because each one changes the shape of the rest.

| | decision | why |
|---|---|---|
| **Export rate** | stays **44,100** | SA3 generates at 44.1 and nothing else, and every DAW conforms on import. Adding a second rate to mira's export path buys nothing and adds a conversion mira would have to be good at. |
| **The film's audio** | a **locked reference track**, excluded from export | You do not want dialogue in your stem export, and you never want to discover it there. |
| **Video on the timeline** | **one video track, several clips on it** | A cut arrives in reels, or you score two scenes in one session. One track keeps the picture unambiguous — there is only ever one thing to look at. |
| **Where the picture lives** | its **own window**, floating above mira | `VideoComponent` is a native `AVPlayerView`. It sits *above* JUCE's rendering, so nothing can be drawn over it — no playhead, no blocks, no overlay. A pane inside the canvas would punch a hole through the arrangement. |

And the rule that ties the first two together:

> **The reference audio is locked to its video clip.** They are one object with two faces.
> Moving either moves both; trimming either trims both. There is no gesture that can put
> them out of sync, because a reference that has drifted from its picture is worse than no
> reference at all.

---

## 3. The hard part: which clock is master

Two clocks exist the moment a video is loaded — the **audio device** and **AVPlayer**. They
are not the same crystal and they will diverge. Over a 40-minute reel that divergence is
not theoretical.

**The audio device is master. The picture chases.** The audio device free-runs and cannot be
told to wait; the transport's position is the truth, and the video is slaved to it.

And the decision that removes most of the problem:

> **AVPlayer's own audio is muted, always.** The film's audio is read by mira's mixer as a
> block, like everything else. One audio clock in the system, not two.

That also disposes of the sample-rate question outright: the film's 48 kHz audio goes
through the same per-voice resampler every other file does (`Voice::rateRatio` plus
Catmull-Rom in `renderRange`, [ARCHITECTURE.md §6.3](ARCHITECTURE.md)). Correct pitch,
correct duration, no special case. That resampler is monitoring-grade rather than
mastering-grade — which is exactly right for the one track in the session that is a
reference rather than a deliverable.

### How the chase works — **measured 2026-09-19, and it is simpler than feared**

Phase 0.2 ran a muted `VideoComponent` against a free-running audio device for **700
seconds** of an 11-minute cut, comparing AVPlayer's position to samples the device had
actually consumed — the audio clock, not wall time.

```
start latency        -290.3 ms      constant
worst drift from it     8.1 ms      0.20 frames at 25
mean drift             -2.9 ms
```

**The error is an offset, not a drift.** It settles at −290 ms within the first seconds —
`play()` takes that long to put a frame up — and then stays within ±8 ms of that for the
rest of the reel. The worst value was reached in the first 30 seconds and **never grew
again**: at 30 s it was 8.0 ms, and at 692 s it was 8.1 ms. Nothing accumulates.

That changes the design. A fixed offset is corrected **once**; only a diverging clock has to
be chased forever, and there is no divergence here.

```
on play / locate      seek the video to (transport position − startLatency), then play
every ~500 ms         error = videoPosition − transportPosition − startLatency
  |error| < ½ frame   do nothing                    ← the measured case, always
  |error| ≥ ½ frame   setPlaySpeed(1.0 − k·error)   ← ease back; kept as a safety net
  |error| ≥ 1 second  setPlayPosition(transport)    ← a seek, a stall, something real
on stop               stop the video, leave it parked at the transport position
```

The rate-nudge stays in the plan, but as a **safety net rather than the mechanism** — for
the cases the spike could not produce: a system under load, a seek mid-playback, a drive
stalling. ±8 ms against a 40 ms frame is a fifth of a frame; nobody will see it.

**The start latency has to be measured per machine, not hardcoded.** −290 ms is this
machine with this device at 48 kHz. Phase 1 measures it once at load by seeking to a known
position and comparing, rather than shipping a number that is right on one laptop.

**And a real gap found on the way:** `getVideoDuration()` returned **0.00 for the whole
run**, even after polling for five seconds. `load()` succeeds, playback works, the duration
is simply never reported. Phase 1 cannot use it to decide the clip's length — take that
from the audio reader, or from an `AVAsset` query of our own.

---

## 4. Loading, and what a 40-minute clip actually costs

The worry is RAM. Measured against how mira is already built, RAM is not the problem:

- **Picture** — `VideoComponent` wraps AVPlayer, which streams from disk. Memory is bounded
  by AVFoundation's own buffering, not by the length of the clip.
- **Audio** — mira *never* loads audio into RAM. `AudioFormatReader` plus
  `BufferingAudioSource` read from disk on the reader thread. A 40-minute file costs the
  same to play as a 40-second one.
- **The waveform** — an `AudioThumbnail` at 512 samples per point over 40 minutes is about
  **206,000 points, ≈3.3 MB**. `ThumbnailStore` and `AudioThumbnailCache` already exist.

So the real cost is not memory but **the first read** — and Phase 0.3 says how much:

| | | |
|---|---|---|
| 93 s mp4, internal SSD | 0.24 s | **385× realtime** |
| 705 s mp4, external USB drive | 58.9 s | **12× realtime** |

Extrapolated to a 40-minute film: **6 seconds from the internal disk, 200 seconds from an
external one.** Three and a half minutes is not a wait anyone will sit through silently, and
a cut is exactly the kind of file that lives on the drive it arrived on.

So: background thread, visible progress, and a **cached result keyed to the file** —
`ThumbnailStore` and `AudioThumbnailCache` already exist for this. A freeze on load is the
failure mode to design against, not an out-of-memory.

### Reading the film's audio — **measured 2026-09-19: `.mp4` yes, `.mov` no**

`registerBasicFormats()` registers `CoreAudioFormat`, which advertises this on macOS 15:

```
.m1a .oga .adts .snd .aif .3gpp .aac .caff .ac3 .aiff .3gp2 .w64 .caf .mp1 .flac .mpa
.3gp .mp2 .au .wav .mov .aifc .opus .mp3 .3g2 .m2a .mpg4 .awb .eac3 .mp4 .m4a .ogg
.mpeg .ec3 .loas .latm .xhe .m4b .m4r .amr .sd2 .qt
```

`.mov` is in that list. **It does not work.**

| file | | |
|---|---|---|
| `Absolut_DC90_060826.mp4` | 48 kHz, 2 ch, 92.99 s | read ok, −0.0 dBFS |
| `Absolut_30Ssec_V14.mp4` | 48 kHz, 2 ch, 33.00 s | read ok, −9.9 dBFS |
| `Mermaids+v.mp4` | 48 kHz, 2 ch, 705.24 s | read ok, −14.8 dBFS |
| `2026-04-11 17-46-57.mov` | — | **no reader** |
| `2026-04-11 17-40-00.mov` | — | **no reader** |
| `Mermaids.mov` | — | **no reader** |
| `LS Trailer 07012023 (1).mov` | — | **no reader** |

**4 of 4 `.mp4` succeed, 0 of 4 `.mov`.** All eight are AAC 48 kHz stereo and `afinfo` opens
every one of them, so Core Audio itself decodes the `.mov` files perfectly well — it is
JUCE's reader that cannot, while `canHandleFile` cheerfully says it can.

**An extension list that lies is exactly what a spike is for.** Written as reasoning rather
than measured, Phase 2.1 would have been "point a block at the video file", which works on
every `.mp4` anyone tries first and fails on the first `.mov` an editor sends — and `.mov`
is what an editor sends.

So the reference track **tries the direct read and falls back to a demux**, and **says which
one it did** (convention 6). The fallback is the main path, not the edge case.

---

## 5. Timecode

`hh:mm:ss:ff` as a **ruler mode**, beside the existing seconds and bars. Three things are
needed and the third is the one that gets forgotten:

1. **Frame rate** — 23.976 / 24 / 25 / 29.97 / 30 / 50 / 59.94 / 60. Read from the asset,
   overridable by hand, stored in the `.mira` document.
2. **Drop-frame**, for 29.97 and 59.94 only. It is a *display* convention: the underlying
   time never changes. Get it wrong and you are ~3.6 seconds out over an hour, with nothing
   on screen to say so.
3. **Start offset.** Cuts routinely start at `01:00:00:00` or `10:00:00:00`. If the editor
   says "hit at 10:04:12:08" and mira is counting from zero, **every cue note exchanged is
   wrong** — and both parties will believe they agree.

These belong to the **picture**, not to the application, so they live in the document beside
the video clip and not in a preferences window.

The existing rulers are a precedent to follow rather than a pattern to copy blindly:
`WaveformView::RulerMode` already radio-pairs time and bars. Timecode joins that set.

---

## 6. The document

`Block` gains nothing. A video clip is its own thing:

```cpp
struct VideoClip {
    juce::File file;           // the .mp4/.mov as given; never copied, never re-encoded
    double start = 0.0;        // where it sits on the canvas timeline
    double length = 0.0;
    double sourceOffset = 0.0; // where in the film `start` corresponds to
    // The picture's own clock, for the timecode ruler.
    double fps = 0.0;          // 0 = unread; read from the asset, overridable
    bool dropFrame = false;
    double startTimecode = 0.0;// seconds; 01:00:00:00 is 3600.0
    juce::int64 audioBlockId = 0;  // the locked reference block, or 0 while it loads
};
```

`audioBlockId` is the lock. The reference block is an ordinary `Block` on a reserved lane,
with two differences: it cannot be dragged on its own, and it is skipped by every export
path. Both are enforced where the gesture happens, not by a flag the renderer has to
remember to check.

**Serialised into the same `.mira`**, under a `video` array. A document with no video array
opens exactly as it does today — that is what makes this additive rather than a migration.

---

## 7. Tasks

### Phase 0 — the two questions that change the plan (half a day)

Nothing else is written until both are answered. This is the same discipline
[spike/README.md](spike/README.md) used for the six risky assumptions.

**Done 2026-09-19.** `spike/07_video_sync/` — the answers are in §3 and §4 above, and each
one changed the plan.

- [x] **0.1** `.mp4` reads directly, `.mov` does not, and the extension list claims both.
  → Phase 2.1 needs a fallback and has to say which route it took.
- [x] **0.2** Not drift — a **constant −290 ms start latency**, then ±8 ms for 700 s with no
  accumulation at all. → the chase loop becomes a safety net; the offset is the mechanism.
  Also: `getVideoDuration()` never reports, so Phase 1 must get length elsewhere.
- [x] **0.3** 385× realtime on the internal disk, **12× on an external one** — 200 s for a
  40-minute film off the drive it arrived on. → progress and a cache, not optional.

The spike stays in the tree. It is the only thing that can re-answer these when JUCE, macOS
or the machine changes, and every number above has a date on it for that reason.

### Phase 1 — a video window that follows the playhead

**Built and SEEN RUNNING, 2026-09-19.** A 93 s mp4 opened on the KOAN-PHILP canvas: the
PICTURE track drew it, the picture followed the playhead, two scrub positions gave two
different frames, and the clip came back out of the `.mira`. The numbers below are from
that run, not from the plan.

The one thing the run did **not** prove is the Done-when: a 93-second clip says nothing
about a 40-minute reel. Phase 0.2 measured 700 s with an instrument and found nothing
accumulating; the reel itself is still owed.

- [x] **1.1** `juce::juce_video` added to `src/mira_ui/CMakeLists.txt`, with
  `JUCE_USE_CAMERA=0` — the module also carries camera capture, and a music tool should not
  ask for the camera.
- [x] **1.2** [`VideoWindow`](src/mira_ui/Source/VideoWindow.h) — a floating, always-on-top
  window holding a `VideoComponent` constructed with `false` (no native controls: mira's
  transport is the only transport). Black surround, not mira's grey — everything around a
  frame changes how you read it.
- [x] **1.3** `Canvas ▸ Open Video...`, and `Canvas ▸ Show Picture` for a clip the document
  already holds. A PICTURE track appears above the tracks, with the clip and its length on
  it, in a **saturated violet that is deliberately not one of `laneColour`'s eight** — every
  track colour is a desaturated mid-tone so that eight can sit together without shouting, so
  a saturated hue reads as a different KIND of row before you have read the word. It is the
  one lane that carries no audio, sums into nothing and exports nowhere, and it should not
  look like a track you could mix. It is in the **Canvas** menu rather than File: File belongs to the library window,
  and every other canvas action already lives here.
  The **waveform progress** of the original task moves to Phase 2 — there is no waveform
  until there is a reference track, and 0.3's 200 seconds is that read, not this one.
- [x] **1.4** Start latency measured **at load, on this machine**: play muted from a known
  position, and one second later ask how far the picture actually got. `elapsed − advanced`
  is the offset, added to every locate. If the picture never moves, the note says so and
  sync stays uncompensated rather than silently wrong (convention 6).
  **Measured on this machine at load: 306.9 ms**, against the spike's 290.3 ms — close
  enough to believe both, far enough apart to be glad it is not hardcoded.
- [x] **1.4b** Clip length and frame rate from an `AVURLAsset` query of our own
  ([VideoNative.mm](src/mira_ui/Source/VideoNative.mm)) — **not** `getVideoDuration()`,
  which Phase 0.2 watched return 0.00 for 700 seconds of successful playback. A partial
  answer is kept and said out loud: a length with no frame rate is still a length.
  **Measured: 93.0 s and 25.0 fps**, against `afinfo`'s 92.99 s. The frame rate is Phase
  3's, arriving free.
- [x] **1.5** Stop parks the picture at the transport position; moving the playhead with
  the transport stopped scrubs the picture. Clicking the PICTURE track scrubs too — over
  picture that is the gesture you actually want.
- [x] **1.6** The window remembers its size and position in `ui_settings`, keyed
  `canvas_video_geometry:<document path>` — per project, because where the picture wants to
  sit depends on what you are scoring.
- [x] **1.7** **Verified on screen**, and it found a bug that had nothing to do with video:
  **the entire Canvas menu had been dead since the day it was added.** Every item — Play,
  Fit, Save Canvas, all of them — greyed out with a canvas plainly open. The macOS menu
  bar bakes each item's enabled state in when the menu is BUILT, and nothing told it the
  canvas had opened. `MainComponent` already carries `onMenuStateChanged` for exactly this,
  with a comment describing the identical failure in the Tags/Segments/View menus;
  `showCanvasWindow()` simply never called it. **The user spotted it in the first five
  seconds of looking at the menu** — which is the argument for convention 8 in one line.
- [ ] **1.8** The Done-when: a **full 40-minute reel**, picture against playhead, drift at
  the end under one frame. 93 seconds does not test this.

**Done when** the picture follows the playhead over a full 40-minute reel and the drift at
the end is under one frame.

**The clip is in the document.** `.mira` grows a `video` array (§6); a document without one
opens exactly as it did, which is what makes this additive rather than a migration. An undo
that leaves the same film in place does **not** reload it — reopening a 40-minute file to
undo a fade would be a three-minute undo.

### Phase 2 — the reference track

- [ ] **2.1** The film's audio as a block on a reserved lane: **try `createReaderFor` first**
  (works for `.mp4`), **fall back to an `AVAssetReader` pass** that writes a wav beside the
  project (needed for `.mov`), and **log which route ran**. Never a silent fallback — the
  two have very different load times and a user who cannot tell them apart cannot explain
  why one film took three minutes to open and another took none.
- [ ] **2.2** **Locked to its clip**: moving or trimming either moves or trims both. Enforced
  in `mouseDrag`, so there is no gesture that can separate them.
- [ ] **2.3** Excluded from `promptExport` in all three modes, and from "export every track".
  A test that proves the exclusion, not a comment claiming it.
- [ ] **2.4** Its lane header says what it is — `REFERENCE`, not `track 4` — and has no
  generator.
- [ ] **2.5** It still has a fader, a mute and a meter. Scoring against picture means
  riding the reference under the cue constantly.

**Done when** exporting every track gives you your stems and no dialogue.

### Phase 3 — timecode

- [ ] **3.1** `hh:mm:ss:ff` ruler mode, radio-paired with seconds and bars.
- [ ] **3.2** fps read from the asset; a menu to override it.
- [ ] **3.3** Drop-frame for 29.97 and 59.94, off elsewhere and not offered there.
- [ ] **3.4** Start-offset field, defaulting to `00:00:00:00`, stored per clip.
- [ ] **3.5** The transport clock reads timecode when the ruler does, so the number you say
  out loud and the number on the ruler are the same number.
- [ ] **3.6** A block's header shows its **in** timecode.

**Done when** a hit called at `10:04:12:08` can be found without arithmetic.

### Phase 4 — several clips on the one video track

- [ ] **4.1** More than one `VideoClip`, non-overlapping, on the single video track.
- [ ] **4.2** The window switches source as the playhead crosses a boundary. **Pre-load the
  next item** — an `AVPlayerItem` swap at the boundary is visible, and a black frame at
  every reel change is the kind of thing that makes a tool feel broken.
- [ ] **4.3** Each clip keeps its own fps and start timecode. Two reels at different rates
  is a real thing.
- [ ] **4.4** The gap between clips is black, not the last frame held.

### Phase 5 — scoring conveniences

Only after 1–4 are real. Listed so they are not forgotten, not to be started early.

- [ ] **5.1** Markers on the timeline at timecodes, with names — the spotting notes.
- [ ] **5.2** "New block at the playhead, as long as the gap to the next marker."
- [ ] **5.3** Snap a block's start to a marker.
- [ ] **5.4** Export a cue sheet: block name, in/out timecode, key, tempo.

---

## 8. Risks, named up front

- ~~**AVPlayer drift may be worse than `setPlaySpeed` can hold.**~~ **Measured and closed:**
  ±8 ms over 700 s, a fifth of a frame, with no accumulation. The risk that remains is the
  *offset* being machine-dependent, which 1.4 handles by measuring rather than hardcoding.
- **`VideoComponent` is a native view and cannot be drawn over.** Accepted, and the reason
  the window is separate. If an overlay is ever needed — a frame counter burned in, say —
  it needs a second native layer, not a JUCE component.
- **Thumbnailing off an external drive is slow** — 12× realtime, so 200 s for a 40-minute
  film. Measured, not feared. The answer is a cache and visible progress; if it turns out to
  be the AAC decode rather than the drive, the demux fallback doubles as the fix, since a
  wav beside the project thumbnails at disk speed.
- **Frame-accurate is not sample-accurate.** mira will get you to the frame. A hit that has
  to land on a specific sample is a DAW's job, and this document does not pretend otherwise.
- **The reference track is one more thing that can clip the master.** It sums with
  everything else. The master meter already exists for exactly this reason.
