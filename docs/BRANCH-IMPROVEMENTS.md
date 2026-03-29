# Branch improvements (vs `main`)

This document summarizes substantial work ahead of or diverging from `main`. **Claims below were checked against the current tree** (paths and symbols grep’d / read); re-run the git commands when you need an exact commit list.

**Maintenance:** When you add or change indexing, `::` region filters, or aggregate/tabulate behaviour, update this file in the same change so it stays a reliable map of “what’s in the code.”

Inspect the committed delta yourself:

```bash
git fetch origin
git log main..HEAD --oneline
git diff main...HEAD --stat
git diff main...HEAD --name-only
```

---

## 1. Build system (`CMakeLists.txt`)

Verified:

- **`PANDO_CWB_DIALECT`** (default `ON`): builds `pando_cwb_dialect` with CWB sources, or `cwb_translate_stub.cpp` when `OFF`.
- **`PANDO_PMLTQ_DIALECT`** (default `ON`): builds `pando_pmltq_dialect` with PML-TQ sources, or stub when `OFF`.
- **`pando_core`** links both dialect static libraries.
- **PML-TQ gold:** `cmake/pmltq_gold_paths.cpp.in` → generated `pmltq_gold_paths.cpp` (default script path substituted at configure time).

**Not present:** There is no `src/query/field_read.cpp` (or `field_read.h`) in this tree and CMake does not reference it. Any earlier note about shared `field_read` helpers was planned or lived only outside the repo.

---

## 2. CWB / CQP dialect (`src/query/dialect/cwb/`)

Verified in CMake: `cwb_lexer.cpp`, `cwb_parser.cpp`, `cwb_translate.cpp`, stub when dialect off, `cwb_dialect_test.cpp` when testing enabled.

**CLI:** `--cql cwb` → `translate_cwb_program` in `src/cli/query_main.cpp`.

---

## 3. PML-TQ dialect (`src/query/dialect/pmltq/`)

Lexer/parser/AST, native lowering, gold JSON path, `pmltq_translate.cpp`, stub when off; test target when `BUILD_TESTING` and dialect ON.

**API (headers):** `translate_pmltq_program`, `translate_pmltq_export_click_sql` (see `pmltq_translate.h`).

**CLI:** `--cql pmltq`; `--pmltq-export-sql` (SQL-only path, see `query_main.cpp` help and error messages).

---

## 4. CLI / CQL driver (`src/cli/query_main.cpp`)

Verified:

- `--cql native|cwb|pmltq`.
- **`tabulate`** (CWB-style): optional `offset` and `limit`, optional query name, then fields. Defaults: offset `0`, limit `1000` when omitted (`GroupCommand` in `ast.h`, parsing in `parser.cpp`). Rows are sliced at **emit** time (`emit_tabulate`) so post-filters still run on the full hit set first.
- **`freq`:** IPM uses **full corpus token count** as denominator (`corpus.size()`), not per-region strata. JSON rows include **`count`**, **`pct`** (percent of `total_matches`), and **`ipm`**.
- **`count` / `group`:** With **two or more** `by` fields, JSON uses a nested **`hierarchy`** (outer field = first `by` column, **`children`** for the rest, sorted by count at each level) instead of flat **`rows`** with tab-joined keys. Single-field counts still emit **`rows`** (`core/count_hierarchy_json.cpp`).
- **Global `::` region filters:** `struct_attr` tokens that contain `_` can be written without `match.` or a named anchor, e.g. `:: text_langcode="nld"` — equivalent to `:: match.text_langcode="nld"` (`parse_global_filters` in `parser.cpp`). Documented in `docs/PANDO-CQL.md`.

**Not in code:** JSON fields like `stratum_prefix_fields`, `ipm_denominator`, or `sort_by: ipm` for freq — no matches in `query_main.cpp` / `program_api.cpp`.

---

## 5. Program / JSON API (`src/api/program_api.cpp`)

Verified aligned with CLI for tabulate window metadata (`total_matches`, `offset`, `limit`, `rows_returned`, `rows`) and freq (`pct` + `ipm` + `total_matches`). Multi-field **`count`** uses the same **`hierarchy`** JSON as the CLI.

---

## 6. Corpus build & indexing (`src/corpus/builder.cpp`, `streaming_builder.cpp`, `src/cli/index_main.cpp`)

**CoNLL-U / comments:** Themes in `builder.cpp`: `# newregion text` / text region attrs (`text_*`), comment parsing discipline, reserved keys vs UD `text_<lang>` translation comments (see `is_reserved_text_structural_key`, `close_text_region_if_open`).

**Region attribute reverse indexes (value → region ids):**

- After each named region column is written as `basename.val` + `basename.val.idx` (e.g. `text_langcode`), **`StreamingBuilder::finalize`** calls `build_region_attr_reverse_index(basename)` (`streaming_builder.cpp`).
- **Emitted files** (same `basename` as the `.val` stem): **`basename.lex`** / **`basename.lex.idx`** (sorted distinct string values), **`basename.rev`** (packed `int64_t` region indices, sorted per value), **`basename.rev.idx`** (cumulative offsets per lex id — same pattern as token reverse indexes).
- **`pando-index`** help text mentions these files.
- **Loading:** `StructuralAttr::add_region_attr` optionally mmap’s `.lex` / `.rev` / `.rev.idx` if `.rev.idx` exists (`structural_attr.cpp`). Public API (attr_name is the **short** name, e.g. `langcode`, not `text_langcode`): `has_region_value_reverse(attr_name)`, `count_regions_with_attr_eq(attr_name, value)`, **`region_matches_attr_eq_rev(attr_name, region_idx, value)`** (O(log K) membership in the sorted `.rev` posting for that value), **`token_span_sum_for_attr_eq(attr_name, value)`** (sum of token spans over regions with that value — used for cardinality).
- **Corpora built before this feature** have no `.rev` sidecar files: unsat checks and `::` filters fall back to scanning regions / string compares on `.val`; **`resolve_leaf` for region-attribute `[struct_attr = value]`** still enumerates the full corpus until seed restriction from `.rev` is implemented.

---

## 7. Query executor (`src/query/executor.cpp`, `executor.h`)

Verified (current names / behaviour):

- **Cardinality:** `estimate_leaf` / `estimate_cardinality` use positional `count_of` / `count_of_id` (reverse index), not a separate exported `try_exact_count_fast_path` symbol — **that name does not appear in source**. For **leaf conditions** on region attributes (`struct_attr` with `_`), **`EQ`** uses **`token_span_sum_for_attr_eq`** (capped by corpus size) when a region reverse index exists; otherwise the estimate stays at full corpus size.
- **Aggregation:** `build_aggregate_plan` + `aggregate_buckets` for `count` / `freq by` when the next statement’s `by` fields are plain positional or `region_attr_names()` columns; `decode_aggregate_bucket_key` for output.
- **Performance:** When aggregating, **`agg_per_match_post`** avoids `match_survives_post_filters_for_aggregate` unless anchors, within-having, not-within, containing clauses, position orders, or global **alignment** filters apply; **`::` region filters** are still applied inline in `add_match` via `pass_region_filters`.
- **`::` region filter EQ fast path:** When a reverse index is loaded for the attribute, **`pass_region_filters`** and **`apply_region_filters`** use **`region_matches_attr_eq_rev`** (binary search on sorted region ids) for **`EQ`** instead of reading `.val` and comparing strings; other comparison ops still use `.val`.
- **Global region EQ unsat:** Before enumerating matches, **`global_region_eq_unsatisfiable`** can return an empty `MatchSet` if an **`EQ`** `::` filter can never hold. If a **region value reverse index** is loaded for that attribute, **O(1)** `count_regions_with_attr_eq` is used; otherwise a linear scan over regions (older corpora).

**Not yet implemented:** Using `.rev` to **restrict token seeds** in `resolve_leaf` / `for_each_seed_position` to positions inside regions that match a value (would shrink work when the value exists but is selective).

---

## 8. UD corpus script (`scripts/`)

Verified: `build_ud_corpus.py` documents `--skip-download` and uses `args.skip_download`; `ud_manifest.example.json` present.

---

## 9. Documentation & repo files

Present in tree: `docs/PANDO-CQL.md`, `docs/SAMPLE-CORPUS.md`, `extensions.md`, `README.md`, **this file** `docs/BRANCH-IMPROVEMENTS.md`.

**Not present:** `docs/ROADMAP-STREAMING-AGGREGATION.md` (no such path in current checkout).

---

## 10. Quick reference: dialect CMake targets

| Target | When ON |
|--------|---------|
| `pando_cwb_dialect` | `cwb_lexer`, `cwb_parser`, `cwb_translate` |
| `pando_pmltq_dialect` | PML-TQ sources + generated `pmltq_gold_paths.cpp` |

---

Regenerate or trim this file before release; merge into a mainline changelog if desired.
