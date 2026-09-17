# MIRA GENERATE — plan and task list

**Written 2026-09-17.** Status: **Phases 1-2 built; Phases 3-7 planned.**

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

- [ ] Keep prompts for a cue name, autocompleting from cues already in the project
- [ ] Creates `<project>/<cue>/` if absent; writes the take there
- [ ] Working name `{project}_{cue}_v{n}` assigned at Keep (§3.7)
- [ ] `files.provenance.generation` written with model, LoRA, strength, seed, steps, cfg,
      prompt (§3.6)
- [ ] `files.human.status` = `chosen` | `alt` | `rejected`, editable afterwards
- [ ] Browser tree shows project -> cue -> takes (existing `onLibraryChanged` refresh)

**Exit:** a full session's work lands organised on disk and in the library. **This is the
point at which the feature is useful.**

### Phase 4 — the take stack

- [ ] Session takes listed left, one compact waveform row each
- [ ] Accordion: the selected take expands in place to a large waveform
- [ ] Keep / Discard / cue name on the expanded take
- [ ] Rows survive until kept or discarded; Cleanup still trashes the unkept

### Phase 5 — cut and fade

- [ ] Trim handles on the expanded waveform, written as a `segments` row
- [ ] Fade in/out and gain handles, written to `segments.human` (§3.5)
- [ ] Audition plays the edit, not the raw take
- [ ] Re-opening a kept take restores its edit

### Phase 6 — export

- [ ] Render segment + fades -> wav (`AudioFormatReaderSource` -> `AudioFormatWriter`,
      both already linked via `juce_audio_utils`) **at the take's native 44,100 Hz**.
      SA3 generates at 44.1 kHz and nothing else — `sa3_mlx.py`, `pre_encode_mlx.py` and
      `demo_mlx.py` all hardcode it, and the 0.0928 s latent step *is* 4096/44100. Export
      must never route through the playback path: `WaveformView` resamples live to the
      device rate via JUCE's `ResamplingAudioSource` (an interpolator plus a simple IIR
      low-pass), which is fine for auditioning and is not a mastering SRC.
- [ ] Delivery name from the template, tokens dropped when unsupported (§3.7)
- [ ] Export cue -> one folder; Export project -> all cues, cue-wise
- [ ] Show in Finder on the export (button exists)

### Phase 7 — share

- [ ] `rclone copy` to a configured remote — one shell-out covers Drive, B2, S3, Dropbox
- [ ] Remote configured once in settings; never per-service code in mira

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
- **Version numbering across sessions**: `v{n}` has to look at what is already in the cue
  folder, not at a session counter, or reopening a project restarts at v1.

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
