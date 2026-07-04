# Handoff: H2C multi-filament variant-index contraction regression

**Status:** root-caused at the architecture level; **fix not yet applied.** One diagnostic
measurement (Step 0 below) is still owed before the port, because it is not yet *proven* whether the
undersized arrays come from the missing `_for_multiple_filaments` machinery or from the upstream
PresetBundle expansion. Do that measurement first — it decides how big the port is.

**Target branch (apply the fix here):** `add_h2c_v2`.
**HEAD when this was written:** `c866180256` (`fix(flatpak): add commit hash…`). Do **not** push.

---

## Worktree topology (important — these are all ONE git repo)

All three checkouts share the same `.git` (they are `git worktree`s), so every branch's source is
reachable from any of them via `git show <branch>:<path>`.

| Path | Branch | Built binary? | Role |
|---|---|---|---|
| `/home/dgalants/git/misc/OrcaSlicer` | `add_h2c_v2` | yes (`build/src/RelWithDebInfo/orca-slicer`, Jul 4) | **the one we fix** |
| `../orcaaddh2c` (`/home/dgalants/git/misc/orcaaddh2c`) | `add_h2c_wip` | **no** | user's active working checkout |
| `/tmp/wt_add_h2c` | `add_h2c` | yes (`build/src/RelWithDebInfo/orca-slicer`, Jul 3) | **the oracle** (known-good, used to diff gcode) |

"Oracle" = the known-good reference. The **compiled** oracle lives at `/tmp/wt_add_h2c` (branch
`add_h2c`). `../orcaaddh2c` is the user's working tree but is on `add_h2c_wip` and has no binary.

**Verified:** `add_h2c` and `add_h2c_wip` are byte-identical for the three files this port touches —
`git diff --quiet add_h2c add_h2c_wip -- src/libslic3r/PrintConfig.cpp PrintConfig.hpp Print.cpp` is
clean. So read the reference source from whichever is convenient; run the oracle **binary** from
`/tmp/wt_add_h2c`. `../BambuStudio` holds real BambuStudio (upstream source of this machinery) if a
third opinion is needed.

Also note the sibling task done in this session (already committed on `add_h2c_v2`,
`c866180256`): ported the flatpak commit-hash injection from `add_h2c_wip` (`fe7570f97e`) into
`build_flatpak.sh`. Unrelated to this regression; mentioned only so the HEAD hash makes sense.

---

## Verified baseline (reproduced this session)

Harness at `/tmp/h2c_harness/`; benchy at
`/tmp/h2c_harness/Original_3DBenchy_Orca-H2C-Vortek.patched.3mf`.

```
[v2_current]      variant-oob=200  unsupportedNVT=0  neg/garbage-feed=0
[oracle_add_h2c]  variant-oob=0    unsupportedNVT=0  neg/garbage-feed=0
```

So: the NozzleVolumeType enum fix (item #1, `64e786caf6`+`a7696d79fb`) is confirmed still good (0
unsupported-NVT in both). The remaining regression is the **200** `variant index … out of range`
warnings, all emitted from v2's single overload at `PrintConfig.cpp:10003`. Observed out-of-range
indices are `2, 4, 6` (stride-2) against arrays that only have room for the smaller
per-extruder space.

Repro command:
```bash
V2=build/src/RelWithDebInfo/orca-slicer
out=/tmp/h2c_harness/verify; rm -rf "$out"; mkdir -p "$out"
$V2 --slice 1 --debug 2 --logfile "$out/s.log" --outputdir "$out" \
    /tmp/h2c_harness/Original_3DBenchy_Orca-H2C-Vortek.patched.3mf
grep -c "variant index.*out of range" "$out/s.log"   # v2: 200  → target 0
```

---

## Symptom & impact

`update_values_to_printer_extruders_for_multiple_filaments` logs (200+ on a plain 4-filament PLA
benchy; 344× in the Steam-Deck logs):

```
… option <KEY> variant index N out of range, skipping
```

KEYs are multi-variant **filament** options (the `filament_options_with_variant` set,
`PrintConfig.cpp:~8248/8400`): `filament_prime_volume`, `filament_prime_volume_nc`,
`filament_retract_speed_nc`, `filament_retract_lift_nc`, `filament_ironing_*`,
`activate_air_filtration*`, …

**Not just log noise:** on an out-of-range index the loop does `continue`, leaving
`new_values[f_index]` at its zero-initialized default. Those per-filament options silently become
**0** (prime volume 0, retract speed 0, ironing 0) for the affected filaments. `add_h2c` produces
**zero** such warnings on identical input → v2 regression.

---

## Root cause (architecture-level, confirmed by reading both trees)

`get_index_for_extruder` is **identical** between the branches (diff-verified), so it produces the
same stride-2 `variant_index` values (`0,2,4,6`) in both. The divergence is that **v2 deleted the
nozzle-volume-count machinery** that (a) computes the true variant count and (b) remaps
`nozzle_volume_type` per filament from `filament_volume_map`. Concretely:

### What `add_h2c` has and `add_h2c_v2` lacks
(line numbers on `add_h2c`; all three files identical on `add_h2c_wip`)

1. **`Slic3r::get_extruder_nozzle_stats(const std::vector<std::string>&)`** — free function,
   `PrintConfig.cpp:652`, decl `PrintConfig.hpp:537`. Parses `extruder_nozzle_stats` strings into
   `vector<map<NozzleVolumeType,int>>` using the file-local `s_keys_map_NozzleVolumeType`
   (v2 has that map at `PrintConfig.cpp:571`). **v2 only has the parallel
   `MultiNozzleUtils::get_extruder_nozzle_stats` in `VortekMultiNozzle.cpp:483`**, not in
   `PrintConfig` scope. Port add_h2c's tiny self-contained version **verbatim** (it needs nothing
   from Vortek — avoids a core→Vortek layering dependency; matches add_h2c byte-for-byte).

2. **`DynamicPrintConfig::get_extruder_nozzle_volume_count(int extruder_count, vector<vector<NozzleVolumeType>>& out)`**
   — `PrintConfig.cpp:9976`, decl `PrintConfig.hpp:694`. Returns the total number of distinct
   `(extruder, nozzle_volume_type)` slots (the variant count) and fills `out`. **Absent in v2.**

3. **The 3-arg overload**
   `update_values_to_printer_extruders_for_multiple_filaments(printer_config, int extruder_count, int extruder_nozzle_volume_count, key_set, id_name, variant_name)`
   — def `add_h2c PrintConfig.cpp:10444`, decl `PrintConfig.hpp:697`. The 1-arg overload
   (`add_h2c:10435`) just computes the count via `get_extruder_nozzle_volume_count` and delegates.
   **v2 has only the 1-arg overload** (`add_h2c_v2 PrintConfig.cpp:9819`, ends `10038`).
   Inside the 3-arg body, vs v2's 1-arg body, add_h2c additionally:
   - reads `filament_volume_map` and, when
     `extruder_nozzle_volume_count > extruder_count && !filament_volume_maps.empty()`, sets
     `nozzle_volume_type = (NozzleVolumeType)filament_volume_maps[f_index]` (per-filament volume
     type) instead of v2's per-extruder `opt_nozzle_volume_type->get_at(filament_maps[f]-1)`.
   - indexes options as `int vi = variant_index[f]; if (vi<0) vi=0; new_values[f]=opt->get_at(vi);`
     — clamps negative to 0 and assumes the option is already expanded to variant size. (v2 instead
     added the `>= opt->size()` guard that skips+warns — the very code that surfaces the regression.)

### The v2 architectural change that hides the machinery (new finding)

v2 relocated the `filament_volume_map` / `filament_nozzle_map` derivation out of `Print.cpp` into
`Vortek::PrintHooks::update_filament_maps_to_config` (`VortekPrintHooks.cpp:157`). Compare:

- **oracle `Print::update_filament_maps_to_config(f_maps, f_volume_maps, f_nozzle_maps)`**
  (`add_h2c Print.cpp:3251`): computes `filament_volume_map` (3258-3296), computes
  `extruder_volume_type_count` via `get_extruder_nozzle_volume_count`, **then** calls the **3-arg**
  overload at `3298`.
- **v2 `Print::update_filament_maps_to_config(f_maps)`** (`Print.cpp:3204`, single arg): calls the
  **1-arg** overload at `3213` **before** `filament_volume_map` exists, then runs the Vortek hook at
  the end (`Print.cpp:3241`).
- The Vortek hook itself (`VortekPrintHooks.cpp:298-306`) *does* populate `filament_volume_map` into
  both `m_full_print_config` and `m_ori_full_print_config`, and *then* (`:329`) calls the **1-arg**
  overload — but the 1-arg overload ignores `filament_volume_map` entirely, so the freshly-computed
  map is never used for indexing. `VortekPrintHooks.cpp:622` (`compute_vortek_derived_maps`) has the
  same pattern.

**Consequence for the fix:** if the 1-arg overload is made to *delegate* to the 3-arg (compute the
count + read `filament_volume_map` itself), then **all four call sites are fixed with no per-callsite
edits** — Print.cpp:3213, PrintApply.cpp:1177, VortekPrintHooks.cpp:329, :622 all keep calling the
1-arg form and transparently get the correct behavior. This is **simpler and lower-risk** than the
original plan's "edit Print.cpp:3213 to call the 3-arg overload." Prefer the delegation approach.

Call sites confirmed via clangd (`findReferences` on the function) — authoritative, 6 refs / 5 files:
`PrintConfig.cpp:9819`(def) + `PrintConfig.hpp:700`(decl) + `Print.cpp:3213` + `PrintApply.cpp:1177`
+ `VortekPrintHooks.cpp:329` + `:622`.

### STILL UNMEASURED — do Step 0 before assuming the port is sufficient

It is **not yet proven** whether restoring items 1-3 is enough. The stride-2 indices `0,2,4,6`
require the option arrays to already be **expanded to variant size** (≥7 here) *before* this function
runs. That expansion is done upstream by the *other* function `update_values_to_printer_extruders`
(no `_for_multiple_filaments`) in `PresetBundle.cpp` (`:126/150`, `:3940/4034` in both branches). Two
possibilities:

- **(A)** arrays ARE expanded identically in both branches, and the only bug is that v2's overload
  contracts/skips instead of remapping → items 1-3 fix it completely.
- **(B)** v2's arrays are actually *shorter going in* (expansion path differs, or the CLI 3mf-load
  path skips it) → items 1-3 will NOT silence the guard, and the real fix is upstream in the
  PresetBundle expansion.

Step 0 distinguishes these. Do not skip it.

---

## Step 0 — diagnostic measurement (build once, ~link-only with ccache)

Add a temporary log at the top of the `switch` in **both** overloads (v2's 1-arg body and the
oracle's 3-arg body) printing, per key, `opt->size()` and the max `variant_index`, then build both
`PrintConfig.cpp.o` and diff:

```cpp
BOOST_LOG_TRIVIAL(warning) << "SIZEPROBE " << key << " opt_size=" << opt->size()
    << " filament_count=" << filament_count;   // put once per case, or before the switch
```

Run the benchy on each binary and compare `SIZEPROBE` lines.
- If v2 `opt_size` == oracle `opt_size` (both ≥7) → case **(A)**, proceed with the port below.
- If v2 `opt_size` < oracle `opt_size` → case **(B)**; stop and investigate the PresetBundle
  expansion path (`update_values_to_printer_extruders`) and the CLI config-load path instead.

(Faster than a full build: `ninja -C build -f build-RelWithDebInfo.ninja \
src/libslic3r/CMakeFiles/libslic3r.dir/RelWithDebInfo/PrintConfig.cpp.o` then relink.)

Remove the probe before committing.

---

## Porting plan (assuming Step 0 = case A) — recommended order

1. **`get_extruder_nozzle_stats`** — port add_h2c's free function verbatim into `PrintConfig.cpp`
   (place near the existing `get_extruder_ams_count`/`save_extruder_ams_count_to_string` at
   `v2 PrintConfig.cpp:627/649`). Add `extern` decl to `PrintConfig.hpp` (near line 537 region).
   Do **not** route through Vortek — keep it self-contained like add_h2c.
2. **`get_extruder_nozzle_volume_count`** — port verbatim (member of `DynamicPrintConfig`), decl in
   `PrintConfig.hpp` (member, ~line 694 region).
3. **Split the overload into two**, exactly as add_h2c (`PrintConfig.cpp:10435` 1-arg delegator +
   `10444` 3-arg body; decls `PrintConfig.hpp:697`+`701`). In the 3-arg body port the
   `filament_volume_map`-based `nozzle_volume_type` remap and the `vi`-clamp indexing.
   **Keep v2's `>= opt->size()` guard as belt-and-suspenders** *after* the `if(vi<0)vi=0;` clamp:
   with arrays correctly sized it must never fire; if it still fires, sizing is still wrong
   (→ case B, investigate, do not just silence).
4. **Do NOT edit the call sites** (this is the change from the old plan): leave Print.cpp:3213,
   PrintApply.cpp:1177, VortekPrintHooks.cpp:329/622 all calling the **1-arg** overload. Delegation
   handles them. Rationale: matches add_h2c per-callsite mix (add_h2c PrintApply.cpp:1193 also stays
   on the 1-arg form) and avoids threading `extruder_volume_type_count` through v2's Vortek path.
   *Only* if Step 0/verification shows Print.cpp:3213 still wrong (because it runs before
   `filament_volume_map` is populated) consider mirroring add_h2c's richer `Print.cpp:3251` body —
   but that is a larger change to v2's `update_filament_maps_to_config` signature; treat as a
   fallback, not the default.
5. Build the single target, run the verification gate.

### Watch out for
- v2 deliberately refactored multi-nozzle code into `Vortek*`. Do **not** assume an add_h2c symbol
  still exists in v2 core — check first (that's how the `get_extruder_nozzle_stats` relocation was
  found). Use clangd `workspaceSymbol`/`goToDefinition` to confirm.
- This is core config code touched by **every** print (all printers). A mistake corrupts non-H2C
  slicing. Keep changes byte-faithful to add_h2c; run the non-H2C sanity slice.

---

## Verification (CLI, headless — no GUI/device)

Binaries: v2 (build it) `build/src/RelWithDebInfo/orca-slicer`; oracle
`/tmp/wt_add_h2c/build/src/RelWithDebInfo/orca-slicer`.

Primary — warnings must drop to 0 on the benchy:
```bash
V2=build/src/RelWithDebInfo/orca-slicer
out=/tmp/h2c_harness/verify; rm -rf "$out"; mkdir -p "$out"
$V2 --slice 1 --debug 2 --logfile "$out/s.log" --outputdir "$out" \
    /tmp/h2c_harness/Original_3DBenchy_Orca-H2C-Vortek.patched.3mf
grep -c "variant index.*out of range" "$out/s.log"   # target 0 (was 200)
grep -c "unsupported NozzleVolumeType" "$out/s.log"   # must stay 0
```

Slice-correctness gate:
```bash
python3 /tmp/h2c_harness/check_structure.py "$out/plate_1.gcode"   # OVERALL: PASS
python3 /tmp/h2c_harness/check_temps.py "$out/plate_1.gcode" \
    /tmp/h2c_harness/Original_3DBenchy_Orca-H2C-Vortek.patched.3mf # SUMMARY: PASS
grep -cE 'F-[0-9]|F[0-9]{10,}' "$out/plate_1.gcode"                # 0 negative/garbage feedrates
```

Per-filament values must match the oracle (this is the whole point — warnings meant values defaulted
to 0). Diff the gcode config block v2 vs oracle:
```bash
/tmp/wt_add_h2c/build/src/RelWithDebInfo/orca-slicer --slice 1 \
    --outputdir /tmp/h2c_harness/oracle \
    /tmp/h2c_harness/Original_3DBenchy_Orca-H2C-Vortek.patched.3mf
diff <(sed -n '/CONFIG_BLOCK_START/,/CONFIG_BLOCK_END/p' "$out/plate_1.gcode") \
     <(sed -n '/CONFIG_BLOCK_START/,/CONFIG_BLOCK_END/p' /tmp/h2c_harness/oracle/plate_1.gcode)
```

Non-H2C regression sanity (**do not skip** — core path): slice any normal single-nozzle 3mf; it must
still slice and produce no `variant index` warnings.

Re-run the multi-mode gate: `/tmp/h2c_harness/variants_v/{nozzle_swap,tool_swap,filament_swap,mixed}.3mf`
(all exit 0 / PASS). `oob_map.3mf` exercises the bad-manual-map path.

### Recreating `/tmp/h2c_harness` if gone
Base 3MFs: `/tmp/wt_add_h2c/tests/h2cvortek/*.3mf`. Patch for CLI (the `-1` sentinels
`raft_first_layer_expansion`, `tree_support_wall_count`, `filament_ramming_volumetric_speed*` must be
made in-range or CLI rejects the file). `tpu_hf_repro.3mf` = the benchy with `project_settings.config`
`nozzle_volume_type` = `["Standard","TPU High Flow"]` (forces the type-3 path). See
[[h2c_cli_slice_testing]].

---

## Build notes (this environment)
- ccache (NOT sccache — warm 25GB ccache here):
  `export CMAKE_CCACHE=ccache CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache`
  `cmake --build build --config RelWithDebInfo --target orca-slicer -- -j24`
- **Run builds in the FOREGROUND.** Background `cmake --build` gets SIGINT-killed in this env (not
  OOM — 100GB+ free). Foreground with a long timeout completes; ninja resumes incrementally.
- A `PrintConfig.hpp` change triggers a wide recompile; ccache makes it mostly a re-link.
- Single object for fast iteration:
  `ninja -C build -f build-RelWithDebInfo.ninja src/libslic3r/CMakeFiles/libslic3r.dir/RelWithDebInfo/PrintConfig.cpp.o`
- Reconfigure if `build/` missing:
  `cmake -S . -B build -G "Ninja Multi-Config" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSLIC3R_STATIC=1 -DSLIC3R_GUI=1 -DBBL_RELEASE_TO_PUBLIC=1 -DCMAKE_PREFIX_PATH="$PWD/deps/build/OrcaSlicer_dep/usr/local" -DCMAKE_INSTALL_PREFIX=build/OrcaSlicer -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`

## Tooling notes — LSP (clangd) vs grep/sed/awk
- clangd **is wired up**: `/usr/bin/clangd` + `build/compile_commands.json` (7MB, Jul 4). The `LSP`
  tool works (tested: `findReferences` returned the 6 authoritative call sites above).
- **Use LSP for semantic navigation in the CURRENT tree:** `findReferences` (exhaustive call sites —
  more trustworthy than grep for "did I get them all?"), `goToDefinition`, `hover` (exact
  `ConfigOption*` element types for the switch cases), `incomingCalls`/`prepareCallHierarchy` (trace
  the Print→Vortek call ordering that matters here), `workspaceSymbol` (confirm whether an add_h2c
  symbol still exists in v2 core).
- **Cross-tree LSP works:** pass a path into `../orcaaddh2c/...`; that tree has its own
  `build/compile_commands.json`, so clangd navigates the oracle (`add_h2c_wip`) too.
- **Caveat — fresh index under-reports:** right after a `cmake` configure, clangd's background index
  is incomplete and `findReferences` misses call sites (queried oracle
  `get_extruder_nozzle_volume_count` returned only def+delegator, missing the real `Print.cpp:3276`
  caller). Let it index, and cross-check completeness with `rg` before trusting a ref list.
- **Keep git/sed/rg for cross-branch work:** LSP indexes only one workspace, so verbatim extraction
  from `add_h2c` and A/B diffing between the two trees still needs `git show <branch>:<file>` + sed +
  `rg`. (User pref: `rg`/`fd` over grep; `rg` rejects `-E`.) Net: LSP for "what/where/who-calls" in
  v2, git+rg for "how does the oracle write it."

## Definition of done
- `variant index out of range` = 0 on the benchy (matches oracle).
- `unsupported NozzleVolumeType` stays 0.
- structure + temps checkers PASS; 0 negative feedrates; per-filament values match the oracle.
- Non-H2C single-nozzle slice unaffected (no new warnings).
- One focused commit; do **not** push. Update memory `h2c_v2_nozzle_volume_type_truncation.md` to
  mark item #2 fixed, and [[h2c_v2_vs_add_h2c_comparison]] if the fix changes the comparison.
