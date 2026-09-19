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

### How the chase works

`juce::VideoComponent` gives us `setPlayPosition`, `getPlayPosition` **and `setPlaySpeed`**.
That last one matters: drift can be corrected by *nudging the rate* rather than by seeking,
so the picture eases back into step instead of jumping.

```
on play / locate      seek the video to the transport position, then play
every ~200 ms         error = videoPosition − transportPosition
  |error| < ½ frame   do nothing
  |error| < 1 second  setPlaySpeed(1.0 − k·error)   — ease back, invisible
  |error| ≥ 1 second  setPlayPosition(transport)    — something went wrong; jump
on stop               stop the video, leave it parked at the transport position
```

**Phase 0 has to measure the real drift before any of these numbers are trusted.** The
thresholds above are a starting guess and are written here to be replaced by measurements,
not defended (convention 2).

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

So the real cost is not memory but **the first read**: building that thumbnail means
reading 40 minutes of audio once. It must be on a background thread with visible progress
and a cached result. A freeze on load is the failure mode here, not an out-of-memory.

### The spike that could remove a whole phase

`registerBasicFormats()` on macOS already registers `CoreAudioFormat`, which asks the system
for the extensions it can open — and that list usually includes `.mp4` and `.mov`. **mira may
be able to read the audio track straight out of the video file**, with no demux, no temp
file, and no new dependency.

If it can, the reference track is just a block pointing at the `.mp4`. If it cannot, Phase 1
needs an `AVAssetReader` pass that writes a wav beside the project once. Ten lines answer it;
nothing else in this plan should be written until it has.

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

- [ ] **0.1** Print `CoreAudioFormat`'s extension list on this machine, then try
  `createReaderFor` on a real `.mp4` and a real `.mov`. Does mira already read film audio?
- [ ] **0.2** Link `juce::juce_video`, put a `VideoComponent` in a bare window, play a
  40-minute clip alongside the canvas transport, and **log the drift every 10 seconds for
  the whole reel**. Answer: how far does it go, does it accumulate linearly, and is
  `setPlaySpeed` enough to hold it?
- [ ] **0.3** Time the first thumbnail of a 40-minute 48 kHz stereo file. If it is minutes,
  Phase 1 needs progress and a cache before it needs anything else.

**Write the three answers into this file before starting Phase 1.** A plan built on 0.1
being "yes" when it is "no" is a plan for a different program.

### Phase 1 — a video window that follows the playhead

- [ ] **1.1** `juce::juce_video` added to `src/mira_ui/CMakeLists.txt`.
- [ ] **1.2** `VideoWindow` — a floating, always-on-top window holding a `VideoComponent`,
  with no native controls (mira's transport is the only transport).
- [ ] **1.3** `File ▸ Open Video...` on the canvas. One video track appears.
- [ ] **1.4** The chase loop of §3, with the thresholds Phase 0 measured. Muted, always.
- [ ] **1.5** Stop parks the picture at the transport position; scrubbing the playhead
  scrubs the picture.
- [ ] **1.6** The window remembers its size and position in `ui_settings`, per project.

**Done when** the picture follows the playhead over a full 40-minute reel and the drift at
the end is under one frame.

### Phase 2 — the reference track

- [ ] **2.1** The film's audio as a block on a reserved lane, by whichever route Phase 0.1
  decided.
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

- **AVPlayer drift may be worse than `setPlaySpeed` can hold.** Phase 0.2 exists to find
  out. If it is, the fallback is a visible correction on a longer interval and an honest
  note in the UI that the picture is a guide, not a lock.
- **`VideoComponent` is a native view and cannot be drawn over.** Accepted, and the reason
  the window is separate. If an overlay is ever needed — a frame counter burned in, say —
  it needs a second native layer, not a JUCE component.
- **Reading audio straight from an `.mp4` may be seek-expensive.** Fine for playback through
  a `BufferingAudioSource`; possibly slow for the thumbnail pass. If 0.3 is bad, a one-time
  wav beside the project is the answer and Phase 2.1 changes route.
- **Frame-accurate is not sample-accurate.** mira will get you to the frame. A hit that has
  to land on a specific sample is a DAW's job, and this document does not pretend otherwise.
- **The reference track is one more thing that can clip the master.** It sums with
  everything else. The master meter already exists for exactly this reason.
