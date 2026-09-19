# CANVAS.md — the block canvas

**Status: experimental, built 2026-09-18/19.** Its own window, its own audio path, one menu
item. Nothing in the generate window, the take stack or the browser depends on it. If it
turns out not to feel good it deletes in one commit and what works today is untouched.

Open it with `Window ▸ Canvas (experimental)`.

---

## 1. What it is

A free surface with a **time axis and no grid**. Blocks of audio sit where you put them,
several sound at once, and nothing asks for a tempo.

The reference is [Blockhead](https://www.patreon.com/colugomusic) — and the first thing to
get right about Blockhead is what it actually does. It is **not** a free 2D canvas: it has
a horizontal time axis like any DAW, with blocks stacked vertically across tracks. What is
radical is the absence of a **grid** — you make something first and derive the tempo
afterwards, if you ever want one.

**That suits mira better than it suits most hosts, because mira measures tempo.** A
generated take arrives with no tempo you chose, and mira works out its BPM, beats,
downbeats and a confidence in that grid (`gridStability`, see
[CLAUDE.md](CLAUDE.md)'s 2026-09-17 entry). "Make first, derive the grid later" is not a
workflow the canvas has to adopt — it is the one mira already has. Nothing draws bars yet;
when it does, they will come from the analysis rather than from a number you typed.

---

## 2. The model

### A block is the unit of both generation and arrangement

This is the whole idea, and it arrived from the user's own framing: *"each block has a
generator, generate the file, keep it and show up in the block."*

| | |
|---|---|
| **exterior** | one piece of audio on a track — what you trim, move, fade and hear |
| **interior** | a private take folder — everything you generated into it |

Only the take you choose reaches the timeline. The alternates stay inside the block.

**That is not tidiness, it is arithmetic.** Stacking alternates is what makes the mix clip:
N takes each peaking near full scale sum to N times full scale — two at −1 dBFS is +5, four
is +11. Keeping alternates inside the block removes the clipping risk by construction
rather than by remembering to mute things.

### A block carries its own properties

A block is not a rectangle that borrows everything from the track under it. It has:

| | |
|---|---|
| **a generator** | the real one, pointed at this block's folder, **with this block's settings** |
| **fades** | dragged on the block, with a shape — linear, equal power, exponential |
| **a mute** | per block, not just per track — an `M` on the block's own header |
| **a colour** | **its own**, kept when it moves to another track |
| **a name** | which is also its folder, and so its generation target |

**The settings belong to the block too.** Selecting a block brings its prompt, its LoRAs
and its numbers with it. The panel used to change only its *title*, so every block appeared
to share one recipe — the same confusion the take stack caused, one level up.

A block the document has no settings for takes them from **its own take's `.json`
sidecar**, the recipe that made exactly that sound. Falling back to whatever was on screen
is what produced the illusion: on a freshly opened project no block had stored settings, so
each one copied the last block looked at, and nothing ever changed but the name. The
panel's current state is now the fallback of last resort — a block with no take and no
sidecar, which is the "new block with the previous block's settings" case.

The settings are captured back out of the panel when you click away from a block and before
a save, so a prompt typed and never clicked away from still reaches the document.

**Colour belongs to the block.** It used to come from whatever track the block sat on, so
dragging a block to another track recoloured it — and the one thing you were following down
a stack changed identity exactly when you moved it. A block takes its colour from the track
it was *born* on and keeps it; the track headers keep their own.

Fades are drawn along the curve the mixer actually applies: `fadeGain()` in
[CanvasEngine.h](src/mira_ui/Source/CanvasEngine.h) is the same function the audio thread
calls per sample. A straight wedge over a sine fade is a picture of something the audio is
not doing, and mira has been caught drawing exactly that kind of lie before.

### The block's length is the duration to generate

A new block is a **30 second frame** — the generator's default — and **resizing it is how
you ask for a different length**. There is no duration to set somewhere else and keep in
step with the picture; the picture *is* the number.

### Cutting is remembered, so you can continue from where the audio really ends

A generated take often ends in silence. Drag the block's right edge **in** to cut it off,
and that cut is kept: dragging back **out** grows the empty tail instead of revealing the
silence you just removed. So "the take trails off, end it there and carry on" is two drags
and a button, not a split, a delete and a re-drag.

`contentSeconds` on the block is what holds this — how much of the *file* the block uses,
as against `length`, how long the block is on the *timeline*. The difference between them
is the tail. A split sets it on both halves, since a cut is a statement about where the
audio ends. **Restore full take** on the right-click menu puts it back; the file was never
touched, only the block's claim about where it ended.

The mixer honours it too: a block is only as long as it sounds, so a fade-out sits at the
end of the audio rather than out in the empty tail fading nothing.

### Extend and remix: inpainting by dragging the block out

Drag a block's right edge **past the end of its audio** and the empty tail is drawn dashed,
with how many seconds it holds. Two buttons fill it:

| | |
|---|---|
| **Extend** | restores the block's own recipe first — the continuation continues in the voice that made what it continues |
| **Remix** | keeps the prompt exactly as you have just typed it |

Everything the inpainter needs is already on screen: **the take is the source, the empty
tail is the range, the block's length is the total.** Nothing to drag in, nothing to line
up, no second timeline to set up by hand — which is what the inpaint strip in the generate
window asks for.

This works because of something verified in `sa3_mlx.py` and recorded on 2026-09-17: **init
audio is zero-padded to the requested duration**, so a range past the end of the audio
generates a continuation, and everything *outside* the range stays bit-exact. Extending is
not a second pass over the whole piece — the audio you already have is untouched.

A block longer than the model will generate is **refused with the number**, not quietly
truncated: audio that stopped short of the frame with nothing on screen explaining why is
exactly the kind of silent failure convention 6 exists to prevent.

### Blocks that overlap on the same track crossfade

Automatically, across the overlap, **equal power** — two linear fades summing through their
middle lose 3 dB and you hear the join as a dip. Only on the same track: blocks on
different tracks are *meant* to sound together, and crossfading those would be the canvas
deciding your arrangement for you.

The crossfade is **computed at mix time, not written onto the block**. Drag the overlap
apart and the fade you drew comes back, rather than leaving behind one you never asked for.
A muted block crossfades with nothing — an inaudible block pulling its neighbour down is
worse than no mute at all.

### A new block always opens on a new track

Hunting for a free gap on an existing track put two unrelated blocks on one fader, and a
track is the thing you mix with — so a block arriving that way arrives already mixed into
something else. Dropped files follow the same rule: four stems land on four tracks at the
same moment, not end to end down one.

Blocks are also **clamped to tracks that exist**. Dragging below the last track used to drop
a block into empty space — a lane with no header, no fader and no mute, which is not a
track, so the block was somewhere you could not mix it from.

### A track is a lane, and lanes just sum

Stacking is the point: drums on one track, guitars on another, playing together. So tracks
have a **level fader, mute, solo and a meter**, and the mixer sums them. There is no
alignment and no shared clock — blocks sound where they sit.

### Everything is a block

A dropped audio file becomes a block like any other: the next `block N` name from one
shared namer, its own folder, and the file **copied into it** as its first take. An earlier
version named the block after the file and left the audio where it was, so the block's
folder was empty and its generator read "no takes yet" — the canvas had grown two kinds of
block, one of them broken.

Copied rather than referenced: the file may be someone else's, and a project that stops
working because a sample was tidied up elsewhere is not a project.

### An empty block is a frame

A block can exist before it has audio — a dashed frame with a length you meant and nothing
in it yet. **Lay out the shape of the piece first, fill it in after.** That is the one
thing generated audio makes possible and no DAW offers.

### Duplicate is a copy, not another version

Same take, same trim, fades, gain and generator settings. Four bars you like become four
bars you like twice. A duplicate that regenerated would be different audio wearing the same
name.

**But it gets a new name, and so a new folder.** The name is the generation target:
`adoptTake` gives a finished take to the block whose folder it was written into, so a
duplicate that kept its original's name sent its generations to the *original* — the first
block found with that folder won. Duplicating now also points the side panel at the copy,
because generating is aimed by whatever the panel is showing. The colour is deliberately
unchanged: colour belongs to the **track**, and the duplicate is on the same one.

### Every generation is kept; only one is shown

A block's folder holds everything ever generated into it, `.wav` and `.json` side by side.
The canvas shows the one in use. Nothing is thrown away and there is no second hierarchy of
takes to manage — the alternates are files in a folder, which is what they are.

---

## 3. The document

**A project is a FILE**, `.mira`, opened and saved like `.npr` or `.als` — not a folder you
point the app at.

That distinction is the whole of "normal DAW behaviour", and mira has already been bitten
by the alternative: opening the *parent* of a project folder succeeded silently and listed
the real project inside it as a cue. A folder is ambiguous — *is this a project, or the
folder containing one?* A document is not.

```
MyProject/
  MyProject.mira        ← the document
  block 1/              ← that block's takes
    block1-s26-2026….wav
    block1-s26-2026….json     the recipe beside the audio
  block 2/
```

Block paths are stored **relative** to the document, so the whole folder can be moved or
renamed. Absolute paths are kept only for files outside it. Takes are not listed in the
document — they are whatever is in the block's folder, so a take added or removed outside
mira is simply seen next time.

Editing marks the document **dirty** (a `*` in the title) rather than writing to disk behind
your back.

---

## 4. The audio

`CanvasEngine.h/.cpp`. Three constraints shape all of it: the audio thread may not
allocate, may not lock, and may not touch the filesystem.

- **The arrangement the audio thread reads is immutable.** Editing builds a new one and
  swaps the pointer; the old one is held in a retired list and freed on the message thread
  once its reference count proves nothing is reading it.
- **File reads happen on a `BufferingAudioSource`'s background thread**, never in the device
  callback. `CanvasAudioSource` does the mixing; the buffering source in front of it is what
  keeps that mixing off the real-time path.
- **Readers are cached across rebuilds**, keyed by path. Reopening twelve files from an
  external drive on every mouse-up is where the lag on dragging a block came from.
- **The timeline is 44,100 Hz regardless of the device.** SA3 generates at 44.1 and nothing
  else; the transport resamples for us.
- Mute, solo and the faders are **atomic**, read directly by the audio thread, so moving one
  takes effect on the next block rather than after a rebuild.

Known and written down: the reference-counted handoff is the JUCE demo pattern. It is
adequate for an experiment, and the pointer assignment itself is not atomic. A lock-free
FIFO is the production answer.

### Three bugs worth remembering

**Every voice was silently skipped.** `if (n > scratch.getNumSamples()) continue;` — a
`BufferingAudioSource` fills its read-ahead buffer in chunks far larger than one device
block (44,100 samples at a time), and the scratch buffer was sized at `blockSize + 8`. Near
silence with the occasional small block getting through: "can't hear anything, it's just
glitching". Rendering is chunked now and the skip is a `jassert` — silently dropping a voice
is what made it inaudible *without saying anything was wrong*.

**Looping restarted every chunk.** The first version rewound `position` at the out point.
That cannot work: `BufferingAudioSource::readBufferSection` calls `setNextReadPosition(P)`
before every chunk with **linear** positions, so once P passed the out point every chunk was
asked for a position beyond it and every chunk restarted at the in point. Looping is a
**pure mapping** now — read from `start + (pos − start) mod span` — which is what
`AudioFormatReaderSource` does, and for the same reason.

**The meter kept counting with nothing playing.** A `BufferingAudioSource` fills its buffer
whether or not the transport runs, so the source went on producing peaks after a stop.

---

## 5. Where this is going

The canvas is meant to **become the project window** once generation through it is proven.
Until then both exist and neither depends on the other — the canvas has to earn the
replacement rather than be handed it.

### Open, in rough order

- [ ] **Levels, properly.** A real fader law and calibrated meters, not a bar and a number.
- [ ] **Sample rate handling for blocks.** Mixed-rate sources currently ride on the
      transport's resampler; export already does the right thing per take
      ([Export.cpp](src/mira_ui/Source/Export.cpp)) and the canvas should match it.
- [ ] **Zoom controls** — buttons and a fit-to-selection, not only `cmd-wheel` and `F`.
- [ ] Gain handle on a block.
- [ ] Bars from mira's own analysis, per block — §1's point, still unbuilt.

### Deliberately not doing

- **Comping by sections.** Choosing the best *moments* across takes needs a real timeline
  inside a block. mira's comping is take *selection*, and calling it comping is already
  generous.
- **Aligning blocks to each other.** No shared clock, no offsets-as-arrangement. The moment
  blocks need to line up, this becomes the takes-lane editor
  ([MIRA-GENERATE.md §6](MIRA-GENERATE.md) puts that out of scope at about a week).
- **Tracktion Engine.** It would give clips, tracks, comping and a timeline for free, and
  reshape mira around itself. Steal the mechanics, not the architecture.

---

## 6. Undo

**Snapshots, not a command log.** The document already serialises to JSON and back, so the
cheapest correct undo is to keep the JSON: there is no per-edit inverse to write, and no
edit that can be added later and quietly forgotten about here. A canvas of a few dozen
blocks is a few kilobytes — nothing beside the audio it points at. Depth 64.

One snapshot per *gesture*, taken as a drag begins rather than per mouse event, or undoing
a slow drag would take fifty presses to get back where you started. Selection is snapshotted
**by name**, because ids are handed out fresh on every load. Undo deliberately does **not**
refit the view: you undid a trim, not a zoom.

**A generation is an edit too.** The wav stays on disk whatever happens, so undo after an
extend means *the block goes back to the take it was showing* — and the new one is still in
the folder if you change your mind. That is the honest answer to "I tried it and I do not
want to keep it"; nothing is deleted, and the arrangement is where it was.

What undo does not cover: the audio device, and anything outside the document.

## 7. Keys

| | |
|---|---|
| `space` | play / stop |
| `L` | loop the selection |
| `F` | fit |
| `M` / `S` | mute / solo the selection's tracks |
| `Cmd-Z` / `Cmd-shift-Z` | undo / redo |
| `Cmd-D` | duplicate |
| `Cmd-E` | split every block the playhead stands on |
| `Cmd-N` / `Cmd-O` / `Cmd-S` | new / open / save |
| `G` / `H` | zoom out / in, around the playhead |
| `shift-G` / `shift-H` | track height, down / up (`+` / `-` too) |
| `cmd-wheel` | zoom, around the mouse |
| shift-wheel | track height |
| wheel | scroll the timeline |
| alt-drag | pan |

**Zoom is on the keyboard because scroll gestures are not the same on every device.** A
trackpad reports `deltaX` and `deltaY`; a mouse wheel reports only `deltaY`, so a pan that
read `deltaX` did nothing at all with a mouse — and macOS turns a shift-held wheel into
`deltaX` itself, which broke shift-zoom the other way round. The wheel handler now works
off whichever axis actually moved, proportionally when the gesture is smooth and in steps
when it is notched. `G`/`H` do the same thing on every device and on a laptop with neither
to hand.
| double-click a track name | rename |
| double-click a block | open its generator |
| right-click a block | mute, fade shape, clear fades, duplicate, split, remove |
| the `M` on a block | mute just that block |
| drag a block's right edge in | cut the audio short, and remember it |
| drag it back out | make a tail for Extend / Remix |
| drag a block's top corner | its fade in / out |
