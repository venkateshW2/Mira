# MIRA GENERATE — plan and task list

**Written 2026-09-17.** Status: **Phases 1-5 built; Phases 6-7 planned.**

A generation-and-delivery workflow inside mira: start a project, generate cues, keep the
takes worth keeping, cut and fade them, hand the folder over.

**Nothing in this document changes existing behaviour.** The current Generate window, the
browser, the analysis pipeline and the database schema all keep working exactly as they
do. Everything here is additive — a new folder group, a new window, four new columns'
worth of JSON in fields that are already free-form. If this were deleted tomorrow, mira
would be unchanged.

---

## 1. The workflow

```
File -> New Project      name + folder on disk
                         becomes a folder root under a new PROJECTS group

generate                 take appears in the stack
   Discard               trashed
   Keep    -> name cue   lands in  <project>/<cue>/  and in the library

...repeat per cue...

Export / Show in Finder  the project folder IS the deliverable
```

The organising principle: **the folder layout on disk is the organisation.** There is no
parallel structure inside the database to keep in sync, because the filesystem already
says what belongs to which cue.

```
PROJECTS
 └ Short Film/                     ui_folder_roots row, group_id = PROJECTS
    ├ cue01_opening/
    │   ├ shortfilm_cue01_v1.wav   chosen
    │   └ shortfilm_cue01_v2.wav   alt
    └ cue02_chase/
```

---

## 2. What already exists — read this before estimating anything

Most of this feature is wiring, not building. Verified against the live database and the
source on 2026-09-17:

| thing | where | state |
|---|---|---|
| Folder groups | `ui_folder_groups` | **4 live**: SCORE STEMS, SAMPLES, MUSIC, MUSIC STEMS |
| Folder roots with a group | `ui_folder_roots(path, display_name, scan_complete, group_id)` | live, tree renders from it |
| **Keep already writes to the library** | `GenerateContent::keepResult` | *"'Kept' is defined by the library, not by a local list — the DB row is the record"* |
| Discard, Cleanup, Show in Finder | `GenerateWindow.h` | `discardButton`, `cleanupButton`, `revealButton` |
| Output folder + filename | `GenerateWindow.cpp:245` | defaults to `~/Music/mira-generated` |
| Browser refresh on keep | `Main.cpp:3000` | `onLibraryChanged` already fires |
| Non-destructive spans | `segments(file_id, start_seconds, end_seconds, human)` | **640 rows in use** |
| Free-form user JSON | `files.human`, `segments.human` | established; `human` outranks machine (PRD §11) |
| Provenance JSON | `files.provenance` | 937 files; holds the *analysis* toolchain |
| Prompt builder | `PromptBuilderWindow` | fixed 2026-09-16 (viewport + picker sync) |
| LoRA slots, cfg, apg, seed, steps | `GenerateWindow.h:108,121,127` | all present and wired |

**The genuinely new work is: a fifth group, a second window, cue-naming on Keep, a take
stack, trim/fade handles, and an export renderer.**

---

## 3. Design decisions, and why

### 3.1 A project is a FOLDER, not a collection

Rejected: `ui_collections` with a `parent_id` for project -> cue.

Taken: a project is a real directory, registered as a `ui_folder_roots` row under a new
PROJECTS group. A cue is a subdirectory.

Why the folder wins:

- **Delivery stops being a feature.** "Cue-wise folder upload" is uploading the folder.
- **Show in Finder already works** and points at something meaningful.
- **`rclone copy <project> remote:path`** handles Drive, B2, S3, Dropbox and forty others
  with no per-service code.
- **Backup and handover are a folder copy.** No export step needed to give someone a set.
- The folder tree UI, scanning and analysis all work unchanged.

What collections were going to provide and no longer need to: nothing. Chosen/alt status
moves to `files.human`, which already exists for exactly this kind of user decision.

### 3.2 PROJECTS declares its own category, `projects` — corrected 2026-09-17

`ui_folder_groups`' schema comment is explicit that a group is an **analysis category** —
*"filing a folder under Score Stems declares its files as stems at scan time"* — and warns
against blurring that meaning. This section first concluded that PROJECTS should therefore
declare **`music`**, since generated cues are full pieces.

**That was wrong, and would have been a silent bug.** `category` is not only a description,
it is the group's *identity key*: `FolderTreeView::findOrCreateCategoryGroup` resolves a
category to a group id via `findFolderGroupByCategory`, which is
`... WHERE category = ? ORDER BY added_at LIMIT 1`. The live library already has:

```
1|SCORE STEMS|stems      3|MUSIC|music
2|SAMPLES|(none)         4|MUSIC STEMS|stems_music
```

MUSIC is older, so it wins that lookup. A PROJECTS group under `music` would never have
been created at all, and every new project would have been filed into **MUSIC** instead —
with no error anywhere.

PROJECTS therefore declares **`projects`**, and §3.2's actual intent is unaffected: nothing
routes analysis off `music`. The only category ever read by the pipeline is a `stems*`
prefix (`MainComponent::isStemPath`, `rootWantsStemDeclaration`), so a project folder gets
exactly what was wanted — router-decided content type, no stem declaration, no marker
workflow. The group also gets its own icon, which `music` would have denied it.

### 3.3 Two windows, independent lifetimes

Today, closing the browser kills the Generate window, because
`Main.cpp:4198` does:

```cpp
void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
```

Change: **quit only when the last window closes.**

Two windows rather than one tabbed window because the take stack wants full width beside
the prompt builder, and because browsing the library while generating is a side-by-side
job. They already stay in sync — `onLibraryChanged` fires on Keep and refreshes the
browser, so a kept take appears in the tree with no new plumbing.

### 3.4 The Project window is the Generate window minus the training bench

The existing Generate window keeps `Prepare LoRA dataset...`, the trigger field and the
prepared-datasets list. That is the training bench and it stays.

The Project window omits all three. It is inference only: prompt builder, LoRA slots and
settings, take stack, edit, keep-to-cue.

Do **not** strip the existing window to make the new one. Two faces, one engine.

### 3.5 Edits are non-destructive; render only on export

A cut and its fades are a `segments` row on the take plus JSON in `segments.human`:

```json
{"fade_in": 0.5, "fade_out": 2.0, "gain_db": -1.5}
```

Nothing is written until export. So a cue can be re-cut a week later without regenerating,
the source take is never damaged, and PRD §1's *"no file ever moves"* holds — **export
copies out, the library stays put.**

### 3.6 Generation provenance goes in `files.provenance`, no schema change

`provenance` already means "what produced this state" and currently holds the analysis
toolchain. A sibling key makes a take reproducible six months later:

```json
{"analysis":   { ...existing... },
 "generation": {"model":"sa3-medium", "lora":"gsl-step=4000", "strength":0.8,
                "seed":26, "steps":50, "cfg":4.0, "prompt":"gsl, Genre: ..."}}
```

Zero files carry generation metadata today, so nothing is being displaced.

### 3.7 Naming: working name at Keep, delivery name at Export

```
Keep    shortfilm_cue01_v1.wav                  project, cue, version are known NOW
Export  shortfilm_cue01_120bpm_Amin_v1.wav      bpm and key need analysis, which lands later
```

The project folder is therefore always shareable — never a pile of timestamps — and no
file is ever renamed in place, which would break its database path.

Rules, decided once:

- **Chosen is v1; alts number after it.** No separate `_alt` convention.
- **A missing field drops its token, it never guesses.** No BPM -> `shortfilm_cue01_Amin_v1.wav`.
  That is convention 1: mira's BPM gate already omits when it cannot support a number, and
  the filename tells the same truth.
- **Slugging**: `"Short Film"` -> `shortfilm`, `"A minor"` -> `Amin`, `120.4` -> `120`.
- **Re-export overwrites deterministically.** Never `_1`, `_2`.

---

## 4. Phases

Work top to bottom. Phases 1-3 alone give a usable workflow — generate, organise by cue,
deliver by hand.

### Phase 1 — projects exist

**Built 2026-09-17.** Compiles and runs; the clicking-through below is the user's to do.

- [x] `PROJECTS` as a fifth `ui_folder_groups` row, declaring category `projects` (§3.2),
      created on first use by `FolderTreeView::registerProject` with its own tree icon
- [x] `File -> New Project`: name, then parent location -> create directory -> insert
      `ui_folder_roots` row under PROJECTS -> set as current project -> first scan
- [x] Current project in the window title (`MIRA — <name>`); setting it retargets the
      generate window's output folder, and opening that window picks it up too
- [x] Window lifetime: closing the browser hides it while a generation window is up, and
      the app quits when that last window goes (§3.3). `Window -> Library` brings it back
- [x] `File -> Open Project` for an existing project folder (never re-scans a known root)

**New in the database:** `ui_settings(key, value)`, holding `current_project`. Migration
verified against a copy of the live 2,544-file library — table created, nothing else
touched.

**Exit:** a project can be created, appears in the browser tree under PROJECTS, and
nothing can be generated without a home.

### Phase 2 — the Project window

**Built 2026-09-17**, except the take-stack layout, which is Phase 4's own work.

- [x] `ProjectWindow`: prompt builder, LoRA slots + strength + min/max step, cfg, apg,
      negative prompt, seed, steps, seconds — no trigger field, no encode button, no
      datasets list (§3.4)
- [x] Output folder bound to the current project rather than `~/Music/mira-generated`
- [x] Opens on New/Open Project, and from `Window -> Project Window...`. Requires a
      project: with none it asks for one rather than quietly writing into `~/Music`
- [x] Not always-on-top, unlike the SA3 Generate window — a project window is where the
      work happens, so it sits beside the browser rather than floating over it
- [ ] Layout: take stack left, prompt builder top right, LoRA and settings bottom right
      — deferred to Phase 4, which is what builds the take stack

**How §3.4 was honoured.** "Two faces, one engine" is a `setTrainingBenchVisible(bool)`
flag on the one `GenerateContent`, and `ProjectWindow` derives from `GenerateWindow`
through a protected constructor. Nothing was stripped and nothing was forked: a second
995-line window would have been two things to keep in step, and every generation fix
would have had to be made twice.

Hidden bench controls are given **empty bounds** and their row space reclaimed, not just
`setVisible(false)`. A hidden component still laid out at full size leaves a hole — the
same failure as the prompt builder's clipped fields, which were laid out all along, just
not anywhere visible.

### Phase 2a — takes do not go in the project root

Found by the user on the first real session, 2026-09-17, and **corrected**.

Phase 1 bound the output folder to the project root, so every take — and nine in ten are
discarded — would have piled into the folder that IS the deliverable (§3.1). That is the
thing the folder design was chosen to avoid: *"so the project doesnt build up all files
in one place."*

Raw takes now go to **`<project>/takes/`**, which `Clean up` sweeps exactly as it already
sweeps `~/Music/mira-generated`. Only Keep writes into `<project>/<cue>/` (Phase 3). The
project stays self-contained — one folder holds the whole session — while the cue folders
stay clean enough to hand over.

**Exit:** generation works in a window with no training surface on it.

### Phase 3 — keep into a cue

**Built 2026-09-17.**

- [x] Keep prompts for a cue, as an **editable combo box** of the cues already in the
      project — picking is the common case (ten takes, one cue), typing makes a new one
- [x] Creates `<project>/<cue>/` if absent; moves the take there with its `.json` recipe
- [x] Working name `{project}_{cue}_v{n}` assigned at Keep (§3.7), the version read from
      **what is already in the cue folder**, never a session counter — this answers §5's
      fourth open question: reopening a project next week continues at v4
- [x] `files.provenance.generation` written, merged as a sibling of the analysis
      toolchain via a new `setProvenanceField` (§3.6). `human.$.generated` still written
      too, so anything already reading that keeps working
- [x] `files.human.status` = `chosen` for v1, `alt` after, plus `human.cue`. In `human`
      precisely because it is a judgement and must stay editable (PRD §11)
- [x] Browser tree shows project -> cue -> takes — cue folders are real directories, so
      the existing tree walk and `onLibraryChanged` refresh cover it with no new plumbing

**A take moves, and PRD §1 is not broken.** "No file ever moves" protects the *user's*
library — folders mira was pointed at. A take in `<project>/takes/` is mira's own scratch
output and filing it is the whole point of the button. `Database::moveFilePath` UPDATEs
the row rather than delete-and-reinsert, so segments, collection membership and any
analysis already done survive the move.

**Without a project, Keep is untouched**: register in place, file under "Generated". The
SA3 Generate window behaves exactly as it did.

**Exit:** a full session's work lands organised on disk and in the library. **This is the
point at which the feature is useful.**

### Phase 4 — the take stack

**Built 2026-09-17.** `TakeStack.h`.

- [x] Session takes listed newest first, one compact waveform row each
- [x] Accordion: the selected take expands in place to a large waveform
- [x] Keep / Discard on the expanded take; the cue prompt is Phase 3's, unchanged
- [x] Rows survive until kept or discarded; Clean up still trashes the unkept, and now
      clears their rows first

**One WaveformView, moved — not one per row.** `WaveformView` opens an
`AudioDeviceManager` in its constructor, so a waveform per take would open one audio
device per take. The expanded row leaves a **hole**, and the window's single preview,
drag tile, Keep and Discard are positioned into it (`setHostedComponents` /
`getExpandedContentArea`). The stack knows where the hole is; the window knows what goes
in it.

Rows are painted by one component rather than being a child `Component` each: a row is a
name, a mini waveform and a triangle, with no focus and no controls of its own.

**`lastRecipe` was wrong the moment a second take existed.** Keep wrote whatever had been
generated *most recently*, so keeping take 3 after generating take 4 would have recorded
take 4's seed, LoRA and cfg against take 3. Keep now reads the `.json` sidecar that
travelled with that specific take — which is what the code writing those sidecars already
claimed was the record. A quiet, plausible, unfalsifiable wrongness of exactly the kind
convention 6 exists to stop.

### Phase 4a — the window, rebuilt

Phase 4's layout was wrong, and the first real session showed it: *"resize destroys the
ui - the prompt gets hidden... the buttons are all placed in weird ways, the slider
movement is not smooth, clicking is difficult."* All of it was one cause or another below.

**A floating waveform, and an empty expanded row.** `setHostedComponents` reparented
`preview` and `resultTile` to the stack, and then a later `addAndMakeVisible(preview)` in
the constructor silently took them back — while `resized()` went on giving them the
*stack's* coordinates. A full-width waveform across the top of the window, and the row it
belonged to left blank. Ownership of a child is what decides whose coordinate space its
bounds are in; the two calls have to agree, and only one of them may exist.

**Two containers, both scrolling.** Takes and audio left, everything else right — the
layout §Phase 2 deferred. The old window was a single top-down column that simply ran out
of height, so whatever fell off the bottom was laid out at zero size and disappeared.
That is the same bug as the prompt builder's clipped fields, for the third time. A pane
that scrolls cannot lose a control at any window size. The right pane is measured and
then placed by **one** function called twice, rather than two that have to agree.

**Slider rows 22px -> 26px.** A `LinearHorizontal` slider in a 22px row leaves a track a
few pixels tall: the grab area was smaller than the pointer. The row height *is* the hit
target.

**Grouped and labelled.** `LORA` and `SETTINGS` headings; output folder and filename
together; Generate / Stop / Show in Finder together. `Output folder...` used to sit
between `Add LoRA file...` and `Build prompt...` with nothing to say they were unrelated.

**Default size 720x700 -> 1180x820**, with a floor of 820x520.

### Phase 4b — the LoRA Library

*"add lora take it out of this - let have it in the osx toolbar... a new window called
load loras - we add the loras and name the lora - so they come into the dropdown."*

The default folder already existed and already worked, with 21 checkpoints in it. What
was missing was a **name**: `tar-step20000-epoch833.safetensors` says which run and which
checkpoint and nothing about what it sounds like.

`Window -> LoRA Library...` lists the folder, adds checkpoints to it, and names them.
Names live in `ui_settings` under one JSON object keyed by **filename**, so a checkpoint
copied to another machine keeps its name. Nothing is renamed on disk — PRD §1, and a
renamed checkpoint would break every recipe that recorded it. Added files are
**symlinked**, not copied: they are 38 MB each and the originals live on an external
drive.

The generate window's dropdown shows the name when there is one and the filename-derived
label when there is not — never both, and never a name invented locally.

### Phase 5 — cut and fade

**Built 2026-09-17.**

- [x] Trim from a waveform selection, written as a `segments` row. `WaveformView` already
      had click-drag selection and `getSelectionSeconds()`, so this is a button, not new
      machinery
- [x] Fade in / fade out / gain, written to `segments.human` one key at a time (§3.5)
- [x] Audition plays the edit — trimmed, faded, gained — not the raw take
- [x] Re-opening a kept take restores its edit, and draws the trim on the waveform

**One segment per take is the edit.** Not a list: a take is a single cue, and *"which of
these four segments did you mean?"* is a question export must never have to ask.
Re-trimming moves the **same row** via a new `Database::updateSegmentRange`, because that
row's `human` holds the fades — delete-and-recreate would silently drop the edit every
time a handle moved.

**An edit needs a library row, so Keep comes first.** A take still sitting in `takes/` has
no `files` row, so there is nowhere non-destructive to put a trim. It says so rather than
writing somewhere that will never be read back — convention 6.

**The audition envelope is an approximation, and is labelled one.** It is stepped from the
UI timer through `WaveformView::setPlaybackGain`, not rendered. Good enough to judge a fade
by ear; a fade under about half a second will audibly step. That is a reason for Phase 6 to
render properly, not a reason to trust this for the last word.

### Phase 5a — what using it actually found

Phases 1–5 were built and then used for a session. Everything below came out of that,
which is the only reason any of it is right.

| reported | actual cause |
|---|---|
| "resize destroys the ui, the prompt gets hidden" | one top-down column ran out of height; anything past the bottom was laid out at zero size. **Two scrolling panes.** |
| a waveform floating at the top, the open row empty | `setHostedComponents` reparented `preview` to the stack, then a later `addAndMakeVisible` took it back — while `resized()` kept giving it the *stack's* coordinates |
| "the slider is very difficult to move" | the step sliders were ~40px wide for a range of 1–50: one pixel was worth more than one step |
| the step slider "does nothing above 8" | the window is in **sampler steps**, compared against Steps. At Steps 8 there is no ninth step. The range follows Steps now |
| "new takes don't come there" | `addTake` set `focusedFile` and rebuilt but never *announced* it, so the preview kept the previous file |
| "the playing waveform is smaller" | `WaveformView` spends height on a ruler and transport *inside its own bounds*, so the focused row gave less of its height to the waveform than the static rows did |
| "there is no selection to trim" | the selection worked and had since it was written — drawn as a 10% white wash with no edges, invisible over a bright waveform |
| "the browser fonts changed and are smaller" | **my regression**: popup metrics overridden on `MiraLookAndFeel` itself to fit the LoRA list, which shrank every context menu in the app. Compactness is opt-in now |

**The general lesson.** Four of these were features that existed and worked but could not
be *seen*, and one was a fix for one control applied to the whole app. Convention 8 covers
the first; the second is its own trap — a fix for one list is not a change of house style.

### Phase 5b — the path-normalisation bug

Not part of this plan, found while chasing "analysis does nothing", and it changes an
assumption the export phase depends on: **paths are not comparable with `==`**.

macOS returns the same filename as NFD from `readdir` and NFC from JUCE's directory walk,
so a file whose name contains an accent was invisible to the file table's database
pairing, and `mira analyze` could not find paths the UI handed it. Fixed in
`Database::findByPath` via `mira::pathsEquivalent`; see CLAUDE.md convention 9.

**Phase 6 must not reintroduce it.** Export resolves a take's row from a path, and a cue
folder named after a project with an accent in it is not a hypothetical.

### Phase 6 — export

**Built 2026-09-18.** [Export.h](src/mira_ui/Source/Export.h) / `Export.cpp`, reached from
`Export...` beside `Clean up...` in the project window. 27 headless checks in
[tools/export_check.cpp](tools/export_check.cpp) (`cmake --build build --target
mira_export_check`) — the sample rate on disk and what the fade arithmetic actually did
are only observable in the output file, never in a screenshot.

- Renders go to `<project>/export/<cue>/`, never into the cue folder: that folder holds
  the working files Keep made, and mixed together it stops answering "which of these do I
  send?" — the question §3.7 exists to answer.
- **`discarded` and `export` are excluded from the cue list.** Both are project plumbing
  that happen to be directories; offering either as a cue to Keep into is how a take gets
  filed in the bin by accident.
- Bit depth follows the source, and a float source stays float. One take failing appends a
  line and the rest still render.
- Fades are **linear in amplitude**, matching the wedge the waveform draws. A curve that
  sounds marginally better but does not match the display would make the display a lie
  about the file — and the display is how the fade gets set.
- Two fades longer than the take between them are squeezed proportionally rather than
  multiplied into a dip in the middle.
- Clipping is **reported with its dBFS figure**, never silently normalised away.

- [x] Render segment + fades -> wav (`AudioFormatReaderSource` -> `AudioFormatWriter`,
      both already linked via `juce_audio_utils`) **at the take's native 44,100 Hz**.
      SA3 generates at 44.1 kHz and nothing else — `sa3_mlx.py`, `pre_encode_mlx.py` and
      `demo_mlx.py` all hardcode it, and the 0.0928 s latent step *is* 4096/44100. Export
      must never route through the playback path: `WaveformView` resamples live to the
      device rate via JUCE's `ResamplingAudioSource` (an interpolator plus a simple IIR
      low-pass), which is fine for auditioning and is not a mastering SRC.
- [x] Delivery name from the template, tokens dropped when unsupported (§3.7). BPM and key
      come from `extractCaptionFields`, so an unsupported tempo is absent from the filename
      for the same reason it is absent from the caption (convention 1)
- [x] Export cue -> one folder; Export project -> all cues, cue-wise
- [x] Show in Finder on the export — `revealToUser()` on the folder that was just written

### Phase 7 — share

**Built 2026-09-18.** In the same `Export...` menu, because exporting and sending are one
errand and a separate button would be a second thing to find.

- [x] `rclone copy` to a configured remote — one shell-out covers Drive, B2, S3, Dropbox
- [x] Remote configured once in settings (`ui_settings.rclone_remote`); never per-service
      code in mira. mira never sees a credential — `rclone config` owns that
- **`copy`, never `sync`.** sync deletes at the destination whatever is not in the source,
  and pointing that at the wrong folder once is unrecoverable over a network
- The destination is `<remote>/<project>/<cue>`: a shared drive holds more than one
  project, and `cue01` on its own says nothing
- rclone is looked up in the four usual install prefixes, not via `which` — a GUI app does
  not inherit a login shell's PATH. Missing rclone says so and names the fix
- The configured-remotes list comes from `rclone listremotes`, so the dialog shows what is
  actually available instead of an empty box
- rclone's exit code is reported as-is; "uploaded" over a non-zero exit is the one thing
  this must never say

---

## 5. Open questions

- **Does a cue ever need more than one chosen take?** Current design says one chosen, any
  number of alts. An assembled cue — 2-3 takes crossfaded into one timeline — is the
  takes-lane editor and is deliberately out of scope here (§6).
- **What happens when a project folder is moved or renamed on disk?** `ui_folder_roots`
  stores an absolute path. Existing roots have the same problem, so this is not new, but a
  project is likelier to move than a sample library.
- **Should Keep queue analysis immediately** (so BPM and key are ready at export) or leave
  it to the normal analysis pass? Immediate is better for the workflow, heavier at Keep.
- ~~**Version numbering across sessions**~~ — **settled in Phase 3.** `v{n}` counts the
  `{project}_{cue}_v*.wav` files already in the cue folder, so reopening a project
  continues where it left off.

---

## 6. Out of scope, deliberately

- **The takes lane / assembly editor.** Cutting 2-3 takes into one crossfaded timeline is
  a separate feature, worth roughly a week of JUCE on its own. Phases 5 and 6 give
  single-take trim and fade, which is what the workflow needs first.
- **Any change to training, dataset prep or the existing Generate window.**
- **ACE-Step, the C++ port, hosted inference.** Tracked separately in
  [ACESTEP-INFERENCE-PLAN.md](ACESTEP-INFERENCE-PLAN.md). None of it changes this plan;
  a second engine would appear behind the same window.
- **Video / scoring to picture.** Reaper already does it, and mira's drag-out reaches
  every DAW. Revisit only if the workflow proves it is needed.
