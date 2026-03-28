# Extensions Sketch: Overlap-Aware Regions and Zero-Width Boundary Events

This note sketches a design for extending pando region handling without implementing code yet.

## Goals

- Keep current fast behavior as default.
- Enable overlap-aware semantics only for structures that need it.
- Support multiple zero-width start/end events at the same token boundary where needed.
- Avoid breaking existing CQL queries and corpus formats.

## Current Behavior (Relevant Baseline)

- Structural lookups resolve one region for a structure at a position.
- Region-attribute checks (e.g. `seg_type`) therefore use one value, not "any of all overlapping regions".
- Region anchors (`<x>`, `</x>`) are treated as zero-width constraints bound to neighboring token positions.
- Empty regions are difficult to preserve in a span-only model because spans are token-position based.

## Extension A: Overlap-Aware Structures

### Problem

For structures like `seg`, a token may be inside multiple overlapping regions. Queries such as:

- `[seg_type="NP"]`
- `a.seg_type = "NP"`

are ambiguous if only one containing region is returned.

### Proposed Approach

Add per-structure behavior flags in corpus metadata (and APIs), e.g.:

- `single_hit` (default, current behavior)
- `multi_hit_any` (existential match over all containing regions)
- optional future: `multi_hit_all` (universal semantics)

Recommended initial scope:

- Implement `multi_hit_any` first.
- Keep `single_hit` for most structures (`s`, `text`, etc.).
- Enable `multi_hit_any` for overlap-prone structures (`seg`, discourse layers, editorial layers).

### Query Semantics

- Token restrictions on region attrs (e.g. `[seg_type="NP"]`):
  - `single_hit`: compare against one containing region value.
  - `multi_hit_any`: succeed if **any** containing region at position has matching attr value.
- `within seg`:
  - `single_hit`: current semantics.
  - `multi_hit_any`: token/match counts as within if any containing region of type `seg` satisfies the condition.
- Global restrictions like `a.seg_type`:
  - either keep legacy single value for compatibility
  - or introduce explicit operators/functions for multi values (see below).

### Disambiguating Value-Returning Expressions

Boolean checks can use existential semantics, but value-returning expressions need policy:

- Option 1 (safe): keep `a.seg_type` as legacy single-hit only.
- Option 2: add explicit forms:
  - `any(a.seg_type) = "NP"`
  - `all(a.seg_type) = "NP"`
  - `values(a.seg_type)` for output/tabulation

Recommended: start with Option 1 + `any(...)` operator for boolean/global conditions.

### Performance Expectations

- Current: roughly `O(log N)` per region check.
- Multi-hit: `O(log N + k)` where `k` is number of overlapping regions containing position.
- With per-structure flags, overhead appears only on flagged structures.
- In typical corpora `k` is small; worst cases need guardrails.

### Guardrails

- Hard cap per-lookup overlap scan (`max_overlap_scan`).
- Metrics/logging for average and max `k`.
- Optional warning when structures have extreme overlap density.

## Extension B: Multiple Zero-Width Events at One Boundary

### Problem

At one boundary between two tokens, one may conceptually have:

- `</text>`
- empty `<text></text>`
- `<text>`

A span-only representation cannot faithfully preserve event order and multiplicity at the same boundary.

### Proposed Approach: Boundary Event Stream (Per Structure, Optional)

Add an optional boundary-event index for flagged structures:

- boundary position: between token `i` and `i+1` (plus corpus start/end boundaries)
- event list at boundary:
  - `START(struct, attrs, event_id)`
  - `END(struct, attrs, event_id)`
- preserve input order for deterministic behavior where needed

Per-structure flags:

- `boundary_events=false` (default, current span behavior only)
- `boundary_events=true` (store and query ordered zero-width events)

### Query Semantics With Boundary Events

For flagged structures with boundary events:

- `<x>` means: boundary has at least one `START(x, ...)` event (optionally attr-filtered).
- `</x>` means: boundary has at least one `END(x, ...)` event.
- Optional strict forms (future):
  - ordered event pattern matching at boundary, e.g. `</text> <text>` as event sequence
  - count-sensitive checks, e.g. `count_start(text) >= 2`.

For non-flagged structures:

- keep current start/end span-anchor behavior.

### Representing Empty Regions

With boundary events enabled, empty regions become natural:

- `START(x)` and `END(x)` at same boundary, linked by `event_id`/pairing info if needed.

Span representation can remain for non-empty regions; empty ones may be event-only for that structure.

## Extension C: Dedicated Region Queries and Counting (Token-Independent)

### Problem

Counting region occurrences via token proxies (for example `<text> [] ; size`) is brittle:

- it misses true zero-width regions (no token consumed)
- it behaves poorly with overlap (one token can be in many regions)
- it is hard to compare region rates across subcorpora in a principled way

### Proposed Capability

Add a region-native query/count path that does not rely on token matches.

Possible commands (illustrative syntax):

- `rquery pause`
- `rcount pause`
- `rcount pause by text_genre`
- `rcount pause in Q` where `Q` is a named token-query result defining a subcorpus
- `keyness-reg pause Fr vs En` (optional later)

The exact syntax can differ, but the key point is: evaluate over region/event indexes directly.

### Region Selection Semantics

For region-native counting, each matching region/event contributes one count item.

For span regions:

- select by structure name and optional region attrs (e.g. `pause_type="short"`)
- optional relation to a subcorpus filter:
  - `intersects`: region overlaps subcorpus span
  - `inside`: region fully inside subcorpus span
  - `anchor`: use region start boundary as representative point

For zero-width boundary events:

- count `START`, `END`, or `BOTH`
- preserve multiplicity at same boundary (important for empty and stacked events)
- optional dedup mode by `event_id` where needed

### Subcorpus Comparison

Enable robust cross-subcorpus comparisons for pauses and similar phenomena:

- absolute counts: number of matching regions/events
- normalized rates:
  - per million tokens (for comparability with token-based stats)
  - per thousand boundaries (useful for boundary events)
  - per text/document (optionally median and distributional stats)

This avoids bias from proxying through "first token after region".

### Interaction With Overlap-Aware Mode

- In region-native counting, overlap is natural: overlapping regions count separately by default.
- Optional `distinct` mode can collapse identical spans/attrs when users want deduplication.
- Token-level predicates like `[seg_type="NP"]` remain separate concerns; region-native counts do not require token resolution.

### Interaction With Boundary-Event Mode

If `boundary_events=true` for a structure, region-native counting should prefer event index for zero-width logic.
If only span index exists, counting falls back to span regions (non-empty only).

### Performance Notes

- Region/event counting should be linear in number of candidate regions/events after structure preselection.
- Subcorpus filters should use indexed region spans or boundary ranges, not per-token iteration.
- This makes counting pauses in large corpora practical and predictable.

## Extension D: Multi-Value Token Attributes (Manatee-Style)

### Motivation (Not Fully Implemented in Current Code)

Today, positional attributes are stored and compared as **single strings** per token position. Real corpora often need **lists of values** on one token:

- **Translation / alignment (`tuid`)**: when sentences do not map one-to-one, a token’s `tuid` may need to be a **list of translation-unit ids** (e.g. space-separated in source data).
- **Open taxonomies**: attributes like “semantic class” may carry **several** labels; you still want `[class = "X"]` to succeed if **any** listed value matches.
- **Global alignment**: expressions like `a.tuid = b.tuid` should succeed when the two positions share **at least one** id in common (set intersection), not only when the entire string is identical.

Manatee supports this style of multi-value behaviour; pando should converge toward it for attributes that are configured (or detected) as multi-value.

### Representation

- **Storage**: keep one string per position, with a **documented delimiter** (space is the usual convention; configurable per attribute if needed).
- **Normalization**: trim tokens; ignore empty segments after split.
- **Backward compatibility**: a single value with no delimiter behaves exactly like today’s scalar attribute.

### Query Semantics

1. **Token restrictions** — existential over list elements:
   - `[tuid = "42"]` matches if `"42"` appears as one of the list elements after splitting.
   - Same for `[semantic_class = "animal"]` when `semantic_class` is multi-value.

2. **Equality between named tokens** — intersection (or pairwise “any match”):
   - `:: a.tuid = b.tuid` is true iff the two lists share **at least one** common element (after normalization).
   - Optional stricter form for later: `all(a.tuid) = all(b.tuid)` or exact multiset equality.

3. **Inequality / regex** — define per operator:
   - `!=` might mean “no shared element” or “literal string differs”; pick one policy and document it.
   - Regex: typically “matches **any** list element” or “matches the concatenated string”; prefer **any element** for consistency with restriction matching.

### Exact vs existential (recommended explicit forms)

Default matching should stay **existential** (“any listed value”), because that matches Manatee-style use and overlap `multi_hit_any`. Users often also need **“only this”** semantics:

| Intent | Meaning (sketch) |
|--------|------------------|
| **Existential** (default) | Value appears among many / any overlapping region matches |
| **Singleton** | The attribute has **exactly one** value, and it equals `X` (token list has one element; or exactly one containing region of that type) |
| **Exact set** | The set of values equals `{X}` exactly (no extra labels) |
| **Exclusive** | `X` is present **and** no other value from a known universe (harder; rarely needed at v1) |

Illustrative syntax (to be chosen in implementation; avoid breaking existing queries):

- **Token lists**: `[tuid only "42"]` or `[tuid = "42" & count(tuid) = 1]` — token must be the single id `42`, not `42 99`.
- **Overlap regions**: `[seg_type = "NP" & seg sole]` — only one `seg` region applies, or its type is uniquely `NP` among all overlapping `seg`s (define precisely).
- **Alignment**: `:: exact(a.tuid) = exact(b.tuid)` or `:: a.tuid = b.tuid & sole(a.tuid) & sole(b.tuid)` for “same single id, not one shared id among many”.

Recommendation: ship **existential** first, then add **one** explicit “sole / only / exact-set” operator family so users do not overload `=` with ambiguous meaning.

### Relation to Overlap-Aware Region Attributes

Multi-value **token** attributes and overlap-aware **region** attributes (e.g. multiple `seg` regions) solve **different** indexing problems but **similar user expectations** (“match if any applies”). The implementation can share:

- a small **value-list** parsing helper for delimited strings;
- **existential** comparison helpers used both in `check_leaf` (token) and in multi-hit region checks (region).

They are **not** the same mechanism: token lists live on `.dat` strings; overlapping `seg_type` comes from multiple region rows. Tabulate/count policy must still be explicit (scalar display vs explode vs `values(a.attr)`).

### Tabulate, Count, and `show values`

- **Scalar display default**: `tabulate M a.tuid` could print the **full** stored string (space-separated list), unchanged from a user perspective.
- **Explode mode** (optional): one output row per `(match, list element)` for selected fields, or a dedicated `tabulate explode a.tuid`.
- **`show values`**: may list **atomic** elements after splitting for multi-value attrs, or list raw strings — document which.

### Implementation Notes (Roadmap)

- Mark attributes as multi-value in `corpus.info` (e.g. `multi_value_attrs=tuid,semantic_class`) or via import convention + detection.
- Thread existential matching through `QueryExecutor::check_leaf` and global alignment filters (`a.tuid = b.tuid`).
- Add tests: list vs list intersection, single vs list, restriction `[tuid="x"]` on multi-value field.

## Related gaps and adjacent problem classes (beyond the current extension set)

The extensions above target **interval regions**, **boundary events**, **overlap-aware existential checks**, **multi-value token strings**, and **region-native counting**. Several neighbouring problems remain out of scope or only partially addressed. The notes below incorporate **TEITOK standoff**, **sub-token tiers** (not full ANNIS-style time search), **hyperedges**, and **user-added overlays**.

### 1. Crossing regions, TEITOK standoff, and “overlap”

TEITOK can represent **crossing** annotations that do not come directly from nested XML: **standoff** layers (e.g. errors, editorial markup) are explicitly meant for cases where spans **overlap or cross** in ways XML cannot express inline.

On a **single linear token sequence**, many “crossing” phenomena are still **intervals** `(start,end)` over token indices: two regions may **overlap without nesting** (partial overlap). The sketched **multi-hit** / **region-native** machinery is aimed at **storing and querying many intervals** that share tokens, which is the right direction for that class of standoff import.

What is **not** automatically covered by intervals alone:

- **Discontinuous** constituents (one annotation = several disjoint spans, e.g. “was … not”): not one rectangle; needs **multiple intervals per annotation id** or a **link** representation.
- **Pure ordering of boundary events** at the same slot (already partly addressed by **boundary-event lists**).

So: **standoff → list of labeled intervals** fits the extension design well; **standoff → graphs or discontinuous spans** needs an extra representation (see §5).

### 2. Sub-tokens vs full time / ANNIS-style search

A **continuous time index** (milliseconds, free alignment to signal) pushes toward **ANNIS-like** search: very expressive, but a different **search model** (harder defaults, more user burden). That is **not** assumed here.

A more targeted step for pando is a **sub-token tier**: optional **units below the word token** (morphemes, phonemes, syllables) with their own ordering and attributes, aligned to **parent token(s)**. That matches many **morphologically aware UD** setups that awkwardly split “token” vs “morph” across columns or MWT lines.

Sketch (implementation-agnostic):

- **Layered positions**: e.g. word positions `0..N-1` and morph positions with `parent_word` + order within word; or a second parallel “virtual” corpus linked by ids.
- **Queries**: either restrict to one layer, or allow explicit **up/down** (“morph query lifted to word matches”) with clear semantics.

This stays **discrete** and **index-friendly**, unlike arbitrary real-valued time.

### 5. Relations that are not spans (hyperedges, coreference, discourse)

**Intervals** are enough for “region over tokens”. **Edges** between arbitrary spans (coref, discourse, alignment) are **not** one interval per relation.

Possible approaches (combinable):

1. **Relation table (edge index)**  
   Store tuples `(rel_type, src_start, src_end, tgt_start, tgt_end, label?, attrs…)` or `(rel_id, endpoint_span_ids…)`.  
   Query: **seed** from token match, **join** to edges touching those positions; or dedicated `rquery edge_type=coref` that scans edges.

2. **Annotation ids + span sets**  
   Each **entity** has an `id`; **mentions** are intervals pointing at `id`. Coref = **same id** across mentions. Query: resolve id sets, then expand to spans (graph closure in the query engine).

3. **Minimal graph in corpus**  
   Optional `links.bin` (or similar) with typed edges; executor provides **neighbour** primitives in CQL (future).

4. **Hybrid**  
   Keep pando strong on **token + interval**; export or mirror **heavy** graphs to **ClickHouse** / SQL for analytics, while pando answers **span-local** constraints.

Recommendation for a first step: **(1) or (2)** with explicit **non-goals** for full graph path queries in v1.

### 9. User-added annotations on an existing corpus (“standoff overlay”)

Goal: users **add** layers (their own regions, attrs, or edges) **without rebuilding** the base corpus from scratch each time.

Options in a **pando-centric** setup:

1. **Sidecar overlay corpus**  
   Second directory (or `overlay/` next to base) with **only** new structures + pointers (`base_corpus=…`, `position_map=identity` or stable token ids). At query time: **merge** structural lookups (base attrs + overlay attrs) with defined **precedence** (overlay wins on name clash).

2. **Append-only region/event files**  
   New `.rgn` / event streams registered in `corpus.info` as **overlay layers**; loader merges into a **virtual** `StructuralAttr` view.

3. **External DB (ClickHouse, SQLite)**  
   Store user annotations in a table keyed by `(corpus_id, start, end)` or `token_id`; **pando** query API runs token query in C++, then **joins** with DB for overlay filters. Easiest for **rapid iteration**; **join latency** and **consistency** are operational concerns.

4. **Re-import pipeline**  
   Periodically **materialize** overlay into the main index (batch job). Simplest runtime; less “live” editing.

Recommendation: document **(1)+(2)** as the native story; **(3)** as the pragmatic path when overlays are large or edited often; **(4)** for production snapshots.

### Other items (unchanged in spirit)

3. **Weighted or ranked multi-values** — scores on labels; needs structured storage or parallel attrs.  
4. **Duplicates and order in multi-value lists** — multisets / ordered sequences.  
6. **Quantifiers and negation under overlap** — all / none / ≥n over overlapping regions.  
7. **Identity of paired START/END events** — pairing ids or stack discipline.  
8. **Incremental corpus updates** — rebuild/versioning strategy.  
10. **Cross-field constraints** — richer global `::` solving.

These items are not required for a first useful release of the planned features, but they explain **where** the same “one string / one region index” model will keep breaking and what to design next.

## Metadata Sketch

Possible per-structure config in `corpus.info` (illustrative):

- `structure.seg.mode=multi_hit_any`
- `structure.seg.boundary_events=true`
- `structure.text.mode=single_hit`
- `structure.text.boundary_events=true`
- `structure.pause.query_mode=region_native`
- `structure.pause.count_unit=events`
- `multi_value_attrs=tuid,semantic_class` (illustrative — delimiter and attr list TBD)

Alternative: a sidecar config file if `corpus.info` should stay compact.

## Migration and Compatibility

- Default behavior remains unchanged when no flags are set.
- Existing corpora continue to work.
- New indexes (multi-hit helpers / boundary events) are optional and built only for flagged structures.
- Query parser changes can be minimal if initial release focuses on existential semantics using existing syntax.

## Incremental Implementation Plan (No Code Yet)

0. **Multi-value token attributes** (Manatee-style): delimiter + `corpus.info` flags; existential restrictions; `a.tuid = b.tuid` as set intersection; tests.
1. Add per-structure metadata flags and load path.
2. Implement multi-hit containment lookup API for flagged structures.
3. Wire boolean region-attr checks to existential semantics for flagged structures.
4. Add boundary-event storage/index for flagged structures.
5. Adapt `<x>` / `</x>` evaluation to use boundary events when available.
6. Add region-native query/count commands over spans/events.
7. Add diagnostics: overlap density, boundary event counts, query-time scan stats.
8. Add tests for:
   - nested overlap with conflicting attrs
   - multiple same-structure overlaps at one position
   - boundary with `END + empty START/END + START`
   - counting zero-width pauses globally and inside named subcorpora
   - backward compatibility on non-flagged structures.

## Open Design Questions

- Should `a.seg_type` remain legacy-single, or require explicit multi-value operators when `seg` is multi-hit?
- Do we need strict ordered boundary-event matching now, or only existential `<x>` / `</x>` first?
- Should empty regions exist as both span and events, or event-only?
- Which structures should be flagged by default in TEITOK-driven imports?
- What should default subcorpus relation be for region-native counts (`intersects`, `inside`, or `anchor`)?
- Delimiter and normalisation for multi-value token attrs (space-only vs configurable)?
- For multi-value attrs, exact semantics of `!=` and regex (any vs all vs whole string)?
- Final syntax for `only` / `sole` / `exact` vs helper predicates (`count`, `cardinality`)?

## Concrete Mini-Spec: Region-Native Counting (Initial Release)

This mini-spec is conservative: it defines a small, unambiguous first subset aimed at pause counting.

### Assumptions

- “Region” means a structural span region (start/end over token positions).
- “Zero-width pause marked as a region” is represented as either:
  - a span region with `start==end` (if the import can materialize it), and/or
  - boundary events for `<pause>` / `</pause>` when `boundary_events=true`.
- Counting is additive: each matching region/event contributes `+1` unless `distinct` is requested (future).

### Core Commands (Illustrative Syntax)

The engine should expose two region-native commands:

1. `rcount <regionType> [<attrFilters>]`
2. `rcount <regionType> [<attrFilters>] by <regionAttr>+`

Where:

- `<regionType>` is a structural type name, e.g. `pause` (as created by TEITOK import).
- `<attrFilters>` uses the same “region attribute equals/regex/etc” syntax already used in token-region constraints.
  - Example: `rcount pause pause_type="short"`.
- `by <regionAttr>+` groups counts by one or more region attributes, e.g. `text_genre`.

### Subcorpus Filters (Token-Independent)

Region-native counting must support selecting a subcorpus without relying on “first token after a region”.

Suggested forms:

- `rcount pause ... in <namedTokenQuery>`
  - The named token-query `Q` defines a subcorpus span set.
  - The implementation maps `Q` matches to a set of region-intervals and then checks relation to each candidate pause region/event.
- `rcount pause ... in_text <textAttrFilters>` (optional sugar)
  - Directly constrains to texts/regions without needing a prior token query.

Relation choice for the initial release:
- default: `intersects` (pause region intersects the subcorpus span).

### Zero-Width Counting Units

If `boundary_events=true` for the structure:

- `rcount pause start`: count boundary start events.
- `rcount pause end`: count boundary end events.
- `rcount pause` (no qualifier): count both start and end events (documented policy).

If `boundary_events=false`:
- `rcount pause start|end` is either rejected or mapped to span semantics (`start==pos` / `end==pos`) depending on what the corpus can represent.

### Example Use

- Global pause frequency:
  - `rcount pause`
- Pause frequency by genre:
  - `rcount pause by text_genre`
- Pause start-events within a token-defined subcorpus:
  - `rcount pause start in Matches`

## Implementation Guidelines (and How to Package for the Roadmap)

### Architectural Placement

1. **Index/model layer**
   - Provide an API to retrieve all containing regions for overlap-aware structures:
     - `find_regions_at(pos, structName)` -> list/iterable of region indices that contain `pos`.
   - Provide a boundary-event API for flagged structures:
     - `events_at_boundary(boundaryPos, structName)` -> start/end event list (with attributes).
2. **Query execution layer**
   - Implement a region-native evaluation path that:
     - iterates candidate regions/events by structure type,
     - filters by region attributes,
     - applies subcorpus relation checks,
     - aggregates counts.
3. **CLI/API layer**
   - Add `rcount` (and optionally `rquery`) command plumbing to the existing query engine.
4. **Output/aggregation**
   - Match existing `count`/`freq` conventions for:
     - global counts,
     - grouped counts by one or more attrs.

### Roadmap Package Template (No Local File Needed)

Since this repo clone doesn’t include the main `ROADMAP`, use the following template to create a “package” entry on the dev server:

Package name: `P-REGIONNATIVE-COUNT` (or similar)

Scope:
- Add region-native counting for span regions (`rcount <type>`).

Dependencies:
- Region index must support enumerating regions for a type (span regions already do).
- CLI/API must accept a new `rcount` command.

Deliverables:
- `rcount pause` returns the correct count for a corpus with span-represented pauses.
- `rcount pause by text_genre` groups by region attribute.
- `rcount pause in <namedTokenQuery>` works with a documented default relation (`intersects`).

Non-goals (for this package):
- Ordered boundary-event pattern matching.
- Full overlap-aware semantics for value-returning `a.seg_type` expressions.

Testing:
- Unit tests for region filtering and subcorpus relation.
- Integration tests for a minimal TEITOK pause corpus.

### Suggested Slicing Into Roadmap Packages

- `P0` (parallel track): **multi-value token attributes** — existential restrictions, `a.tuid = b.tuid` as list intersection, corpus metadata for which attrs are multi-value
- `P1`: `rcount` for span regions only (no boundary events, no overlap-aware multi-hit)
- `P2`: boundary-event qualifiers for zero-width (`start`/`end`)
- `P3`: existential overlap-aware region-attr checks for flagged structures (`multi_hit_any`)
- `P4`: performance guards + diagnostics for dense overlaps / high event multiplicity


## Additional high-impact research features (outside current extension scope)

Beyond regions/overlap/multi-value support, several capabilities would likely add major value for linguistic research:

1. **Metadata-aware normalisation in frequency/keyness/collocations**
   - Better denominators for unequal subcorpora (genre, period, speaker, document length).
   - Built-in effect-size and confidence reporting, not only ranking by one score.

2. **Mixed-effects style modeling hooks**
   - Export-ready grouped aggregates (by text/speaker/document) and optional direct interfaces to R/Python workflows for robust inferential stats.
   - This helps avoid over-interpreting raw counts from repeated measures corpora.

3. **Richer query provenance and reproducibility**
   - Save exact query program + corpus version + settings + output schema hashes.
   - Essential for replicable corpus studies and shared methods sections.

4. **Annotation agreement and adjudication workflows**
   - If sidecar manual layers are a key use case, built-in agreement metrics and diff/adjudication support would be very helpful.
   - Especially useful before promoting overlays into the main index.

5. **Lexicon/morphology integration beyond plain attrs**
   - Productive paradigms, derivation families, valency frames, and controlled-vocab mappings as first-class query resources.
   - Supports typology and historical linguistics use cases.

6. **Sequence mining over matched subsets**
   - N-gram and pattern mining constrained by region/metadata/dependency filters.
   - Useful for exploratory research where exact hypotheses are not known upfront.

7. **Inter-annotator and inter-corpus comparability helpers**
   - Normalization and mapping layers to compare corpora with different tagsets/schemes.
   - Complements multi-value attrs and overlap-aware structures.

8. **Result auditing at scale**
   - Fast stratified sampling, error buckets, and active-learning style “show me uncertain/rare cases” views.
   - Helps researchers validate query quality on large datasets.

## Extension E: Pluggable CQL dialect adapters (CWB, Manatee, PML-TQ)

Pando’s **native** query language is pando-CQL (see `docs/PANDO-CQL.md`). Many users arrive with habits from **CWB/CQP**, **Manatee/Sketch Engine**, or **PML-TQ**. A **thin translation layer** can accept those surface syntaxes where feasible, compile to the **existing pando AST**, and reuse the same executor — without claiming to replace those engines at their core strengths.

**Implementation roadmap (phases, tests, other candidates):** [`dev/CQL-DIALECT-ROADMAP.md`](dev/CQL-DIALECT-ROADMAP.md).

### Goals

- **Familiarity**: optional `--cql <dialect>` (or equivalent API flag) selects a dialect parser.
- **Faithful where possible**: map constructs to pando AST; when impossible, emit a **clear error** with a suggested native rewrite.
- **Isolation**: one **independent** parser/lexer module per dialect; no cross-import spaghetti with the native parser.
- **Auditability**: optionally log the **canonicalized** pando AST or program string after translation (for debugging and papers).

### CLI / API sketch

- `--cql native` (default): current behaviour.
- `--cql cwb` | `--cql manatee` | `--cql pmltq`: run the corresponding dialect front-end, then execute as today.

### Architecture

```
[ dialect lexer/parser ] -> [ dialect AST IR ] -> [ translator ] -> [ pando AST (existing) ] -> [ executor ]
```

- **Dialect AST IR**: small, dialect-specific tree (only what that dialect needs).
- **Translator**: pure functions / visitor pattern; unit tests per mapping rule.
- **Feature matrix**: table per dialect listing supported vs “translate with caveat” vs unsupported.

### CWB (CQP) compatibility notes

- **Regex**: CWB uses a different convention for regex in token patterns (often word-boundary semantics). Map to pando’s regex form (`/.../`) with explicit anchors where CWB implies them.
- **Literal fast path**: when the CWB-style pattern is a plain literal (no metacharacters), translate to **equality** (`form="..."`) rather than always lowering to regex — see `dev/CQL-DIALECT-ROADMAP.md` (Phase 1).
- **Structural quirks**: map CWB regions/s-attributes to pando regions / `region_attr` naming; document mismatches.
- **Aggregations**: map `count`, `group`, etc., to pando commands where possible; flag unsupported aggregation shapes early.

### Manatee / Sketch-style notes

- **Surface differences** (operators, macro expansions, sketch-specific helpers): implement as a **Manatee dialect** module that targets the same translator.
- Expect **partial** coverage first; expand based on real query corpora from users.

### PML-TQ notes

- **Tree/query patterns** are often richer than CWB-style token sequences; some may not map cleanly to pando’s token+dependency model.
- **Aggregation** and **server-side** functions may differ: aim for **best-effort** translation with explicit limitations documented.
- Consider a **“PML-TQ subset”** profile that is guaranteed translatable.

### Error handling and diagnostics

- **Structured errors**: `unsupported_construct`, `ambiguous_mapping`, `requires_native_syntax`.
- Provide **rewritten** native query suggestions when trivial (e.g. regex anchoring).
- **Complete dialect parsers**: each front-end should accept the **full** surface grammar (per manual) and either translate, **emit a named unsupported construct** (e.g. CWB `merge` until implemented in pando), or error with operator name — **no silent drops**.
- **`--debug`**: verbose **translation trace** (dialect parse summary, literal-vs-regex classification, resulting pando program / AST, warnings) for batch-testing query batteries; see [`dev/CQL-DIALECT-ROADMAP.md`](dev/CQL-DIALECT-ROADMAP.md) §2.1–2.2.

### Testing strategy

- Golden corpus of **real queries** per dialect (small + growing).
- Round-trip tests where meaningful: dialect parse → translate → print native → compare semantics (not necessarily string-identical).

### Roadmap packaging

- `P-CQL-DIALECT-CWB`: minimal CWB parser + translator + tests + `--cql cwb`.
- `P-CQL-DIALECT-MANATEE`: Manatee profile subset.
- `P-CQL-DIALECT-PMLTQ`: PML-TQ subset + aggregation limitations doc.

This is intentionally **additive**: native pando-CQL remains the source of truth for full feature coverage.
