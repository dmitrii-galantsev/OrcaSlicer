# Handoff: H2C multi-filament variant-index contraction regression

**Status:** root-caused, NOT fixed (deliberately deferred — large cross-module port of core config
machinery that `add_h2c_v2` deliberately refactored; correctness not verifiable headless).
**Branch:** `add_h2c_v2`  **Written at HEAD:** `a7696d79fb` (do not push).
**Reference branch (the oracle, has the working code):** `add_h2c` (worktree at `/tmp/wt_add_h2c`,
binary at `/tmp/wt_add_h2c/build/src/RelWithDebInfo/orca-slicer`).

This is the second of two issues found in the Steam Deck runtime logs (`./deck-logs/`). The **first**
(the dominant 470× `unsupported NozzleVolumeType=3`) is ALREADY FIXED — do not redo it:
- `64e786caf6` restored the `NozzleVolumeType` enum (nvtHybrid=2, nvtTPUHighFlow=3) + name/config maps.
- `a7696d79fb` restored the `nvtHybrid → nvtStandard` normalization in `get_index_for_extruder`.

---

## Symptom

Slicing on an H2C printer logs many (200+ on a plain 4-filament PLA benchy; 344× in the user's logs):

```
update_values_to_printer_extruders_for_multiple_filaments, Line NNNN: option <KEY> variant index N out of range, skipping
```

Affected KEYs are multi-variant **filament** options: `filament_prime_volume`, `filament_prime_volume_nc`,
`filament_retract_speed_nc`, `filament_retract_lift_nc`, `filament_ironing_speed`, `filament_ironing_spacing`,
`filament_ironing_inset`, `activate_air_filtration*`, etc. (the `filament_options_with_variant` set,
`PrintConfig.cpp:~8248/8400`).

**Impact (not just log noise):** when an index is out of range the loop does `continue`, leaving
`new_values[f_index]` at its zero-initialized default. So those per-filament options become **0** for the
affected filaments (e.g. prime volume 0, retract speed 0, ironing 0). `add_h2c` produces **zero** of these
warnings on the identical input, so this is a v2 regression.

The out-of-range indices observed are `0, 2, 4, 6` (stride-2) — i.e. `variant_index` is computed for a
`(extruder × nozzle_volume_type)` variant space, but the option arrays are only sized for the smaller
space, so even indices past the first overflow.

---

## Root cause

`get_index_for_extruder` is essentially **identical** between the branches (verified by diff), so the
`variant_index` values are the same in both. The divergence is that **v2 dropped the stride/nozzle-volume-count
machinery** that (a) sizes the option arrays and (b) remaps `nozzle_volume_type` per filament, so v2's option
arrays are too short for the indices.

### What `add_h2c` has that `add_h2c_v2` is missing

All line numbers are on branch `add_h2c` unless noted.

1. **`get_extruder_nozzle_stats(const std::vector<std::string>&)`** — free function in `Slic3r` namespace,
   `PrintConfig.cpp:652`, declared `PrintConfig.hpp:537`.
   - In v2 this exists ONLY as `Slic3r::MultiNozzleUtils::get_extruder_nozzle_stats`
     (`VortekMultiNozzle.cpp:483`), NOT in `PrintConfig` scope. The port needs it callable from
     `PrintConfig.cpp` (either add a thin `Slic3r::get_extruder_nozzle_stats` wrapper that delegates to
     the MultiNozzleUtils one, or include the Vortek header — prefer the wrapper to avoid a layering
     dependency from libslic3r core onto the Vortek module).

2. **`DynamicPrintConfig::get_extruder_nozzle_volume_count(int extruder_count, std::vector<std::vector<NozzleVolumeType>>& out)`**
   — `PrintConfig.cpp:9976`, declared `PrintConfig.hpp:694`. Reads the `extruder_nozzle_stats` config
   option and returns the total number of distinct `(extruder, nozzle_volume_type)` slots (the "variant
   count"), and fills `out` with the per-extruder volume-type lists. **Absent in v2.**

3. **The second overload**
   `update_values_to_printer_extruders_for_multiple_filaments(printer_config, int extruder_count, int extruder_nozzle_volume_count, key_set, id_name, variant_name)`
   — `add_h2c PrintConfig.cpp:10444`, declared `PrintConfig.hpp:697`. The existing single overload
   (`add_h2c:10435`) just computes the count and delegates to it. **v2 has only the single overload**
   (`add_h2c_v2 PrintConfig.cpp:9819`).

   Inside this overload, vs v2's current body, `add_h2c`:
   - reads `filament_volume_map` and, when `extruder_nozzle_volume_count > extruder_count && !filament_volume_maps.empty()`,
     sets `nozzle_volume_type = (NozzleVolumeType)filament_volume_maps[f_index]` (per-filament volume type,
     instead of v2's per-extruder `opt_nozzle_volume_type->get_at(filament_maps[f]-1)`).
   - indexes options as `int vi = variant_index[f_index]; if (vi < 0) vi = 0; new_values[f_index] = opt->get_at(vi);`
     — clamps negative to 0 and assumes the option is already the right (expanded) size. (v2 instead added
     the `>= opt->size()` guard that skips+warns — which is what surfaces the regression. Note: add_h2c's
     unchecked `get_at` is technically an OOB read if the option is undersized; the real fix must ensure the
     option IS the right size, then the guard is harmless to keep.)

4. **Caller in `Print.cpp`** — `add_h2c Print.cpp:3273-3298` computes
   `extruder_volume_type_count = m_ori_full_print_config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types)`,
   uses it around line 3291 (`(extruder_volume_type_count > extruder_count) && filament_volume_map...`), and
   calls the 3-arg overload at `3298`.
   - v2's caller is `Print.cpp:3213` (single overload) and `PrintApply.cpp:1177` (single overload).
   - Also check the Vortek callers `VortekPrintHooks.cpp:329` and `:622` — decide whether they should use
     the richer overload too, or stay on the single one (add_h2c's `PrintApply.cpp:1193` still uses the
     single overload, so mixed usage is expected — match add_h2c per-callsite).

### Inputs already present in v2 (no need to add)

- `extruder_nozzle_stats` — `ConfigOptionStrings`, `PrintConfig.hpp:1522`. Accessible via
  `config().option("extruder_nozzle_stats")`.
- `filament_volume_map` — `ConfigOptionInts`, `PrintConfig.hpp:1531`. Accessible via
  `config().filament_volume_map`.
- `support_different_extruders`, `get_index_for_extruder` — present and (get_index_for_extruder) identical.

---

## Porting plan (recommended order)

1. Make `get_extruder_nozzle_stats` callable from `PrintConfig.cpp`. Preferred: add a small
   `Slic3r::get_extruder_nozzle_stats(const std::vector<std::string>&)` in `PrintConfig.cpp` that delegates
   to `MultiNozzleUtils::get_extruder_nozzle_stats` (add the `extern` decl to `PrintConfig.hpp`). Avoid a
   core→Vortek link dependency if the build layering forbids it — if it does, copy the tiny parser instead
   and note the duplication.
2. Port `DynamicPrintConfig::get_extruder_nozzle_volume_count` verbatim from `add_h2c PrintConfig.cpp:9976`
   (+ decl `PrintConfig.hpp:694`).
3. Split v2's `update_values_to_printer_extruders_for_multiple_filaments` into the two overloads exactly as
   `add_h2c` has them (`PrintConfig.cpp:10435` + `10444`, decls `PrintConfig.hpp:697/701`). Port the
   `filament_volume_map`-based `nozzle_volume_type` remap and the `vi`-clamp indexing. **Keep v2's
   `>= opt->size()` guard as a belt-and-suspenders** (with the arrays now correctly sized it should never
   fire; if it still fires, the sizing is still wrong — investigate before removing).
4. Update the `Print.cpp:3213` caller to compute `extruder_volume_type_count` and call the 3-arg overload,
   mirroring `add_h2c Print.cpp:3273-3298` (including the `filament_volume_map` write-back around 3288-3292).
   Leave `PrintApply.cpp:1177` on the single overload (matches add_h2c:1193) unless verification shows it
   needs the richer one.
5. Build (see Build notes) and run the verification gate below.

### Watch out for

- v2 deliberately refactored a lot of multi-nozzle code into the `Vortek*` modules and truncated some
  paths. Do NOT assume a function that add_h2c calls still exists in v2 core — grep first (that's how the
  `get_extruder_nozzle_stats` relocation was found).
- The option arrays are *expanded* to variant size upstream in `PresetBundle.cpp` (both branches call
  `update_values_to_printer_extruders(..., filament_options_with_variant, "", "filament_extruder_variant", ...)`
  at `PresetBundle.cpp:126/150` and `:3940/4034`). If, after porting steps 1-4, the guard still fires,
  the real undersizing is in that expansion path — compare the option sizes at the top of
  `update_values_to_printer_extruders_for_multiple_filaments` between the two binaries (add a temporary
  `BOOST_LOG_TRIVIAL(warning) << key << " size=" << opt->size()` and diff).
- This is core config code touched by EVERY print (all printers, not just H2C). A mistake can corrupt
  non-H2C slicing. Keep the changes byte-faithful to add_h2c and run the non-H2C sanity slice too.

---

## Verification (CLI, headless — no GUI/device needed)

Harness lives in `/tmp/h2c_harness/` (recreate if gone — see below). Binaries:
- v2 (build it): `build/src/RelWithDebInfo/orca-slicer`
- add_h2c oracle: `/tmp/wt_add_h2c/build/src/RelWithDebInfo/orca-slicer`

Primary check — the warning count must drop to add_h2c's (0) on the plain benchy:
```bash
V2=build/src/RelWithDebInfo/orca-slicer
out=/tmp/h2c_harness/verify; rm -rf "$out"; mkdir -p "$out"
$V2 --slice 1 --debug 2 --logfile "$out/s.log" --outputdir "$out" \
    /tmp/h2c_harness/Original_3DBenchy_Orca-H2C-Vortek.patched.3mf
grep -c "variant index.*out of range" "$out/s.log"   # target: 0 (was ~200)
grep -c "unsupported NozzleVolumeType" "$out/s.log"   # must stay 0 (enum fix)
```

Slice-correctness gate (must still pass — the fix changes real filament values, so confirm output is sane):
```bash
python3 /tmp/h2c_harness/check_structure.py "$out/plate_1.gcode"   # OVERALL: PASS
python3 /tmp/h2c_harness/check_temps.py "$out/plate_1.gcode" \
    /tmp/h2c_harness/Original_3DBenchy_Orca-H2C-Vortek.patched.3mf  # SUMMARY: PASS
grep -cE 'F-[0-9]|F[0-9]{10,}' "$out/plate_1.gcode"                 # 0 negative/garbage feedrates
```

Cross-check the actual per-filament values are no longer 0 where the profile has real values (this is the
whole point — the warnings meant prime/retract/ironing defaulted to 0):
```bash
# e.g. filament_prime_volume / retract entries in the gcode config block should match the presets,
# and diff the v2 output against the add_h2c oracle output of the same 3mf:
/tmp/wt_add_h2c/build/src/RelWithDebInfo/orca-slicer --slice 1 --outputdir /tmp/h2c_harness/oracle \
    /tmp/h2c_harness/Original_3DBenchy_Orca-H2C-Vortek.patched.3mf
diff <(sed -n '/CONFIG_BLOCK_START/,/CONFIG_BLOCK_END/p' "$out/plate_1.gcode") \
     <(sed -n '/CONFIG_BLOCK_START/,/CONFIG_BLOCK_END/p' /tmp/h2c_harness/oracle/plate_1.gcode)
```

Non-H2C regression sanity (core code path — do not skip): slice any normal single-nozzle 3mf and confirm
it still slices and the warning does not appear.

Also re-run the multi-mode gate that the earlier session used (all should stay exit 0 / PASS):
`/tmp/h2c_harness/variants_v/{nozzle_swap,tool_swap,filament_swap,mixed}.3mf`.

### Recreating the harness if `/tmp/h2c_harness` is gone
The base 3MFs come from the add_h2c worktree tests: `/tmp/wt_add_h2c/tests/h2cvortek/*.3mf`. Patch them for
CLI (the `-1` sentinels `raft_first_layer_expansion`, `tree_support_wall_count`,
`filament_ramming_volumetric_speed*` must be made in-range, else CLI rejects the file) — the earlier session's
`patch_3mf.py`/`make_variant.py`/`check_structure.py`/`check_temps.py` did this; regenerate equivalents if
missing. `tpu_hf_repro.3mf` = the benchy with `project_settings.config` `nozzle_volume_type` set to
`["Standard","TPU High Flow"]` (forces the type-3 path).

---

## Build notes (this environment)

- Use ccache and build the single target:
  `export CMAKE_CCACHE=ccache CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache`
  `cmake --build build --config RelWithDebInfo --target orca-slicer -- -j24`
- **Run builds in the FOREGROUND.** Background `cmake --build` kept getting SIGINT-killed in this env
  (NOT OOM — 100+GB free). Foreground with a long timeout completes; ninja resumes incrementally if a run
  is interrupted.
- A `PrintConfig.hpp` change triggers a wide recompile; ccache makes it fast (mostly re-link).
- If `build/` is missing, reconfigure:
  `cmake -S . -B build -G "Ninja Multi-Config" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSLIC3R_STATIC=1 -DSLIC3R_GUI=1 -DBBL_RELEASE_TO_PUBLIC=1 -DCMAKE_PREFIX_PATH="$PWD/deps/build/OrcaSlicer_dep/usr/local" -DCMAKE_INSTALL_PREFIX=build/OrcaSlicer -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`
- For fast iteration on just the config code, compile the single object first:
  `ninja -C build -f build-RelWithDebInfo.ninja src/libslic3r/CMakeFiles/libslic3r.dir/RelWithDebInfo/PrintConfig.cpp.o`

## Definition of done

- `variant index out of range` warnings = 0 on the benchy (matches add_h2c).
- `unsupported NozzleVolumeType` stays 0.
- structure + temps checkers PASS; 0 negative feedrates; per-filament values match the add_h2c oracle.
- Non-H2C single-nozzle slice unaffected.
- One focused commit; do NOT push. Update memory `h2c_v2_nozzle_volume_type_truncation.md` to mark item #2
  fixed.
