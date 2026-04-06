# Sample corpora

The repository ships **three** practical ways to exercise indexing and queries: a tiny **CoNLL-U** file (UD), a **JSONL** structural/multivalue fixture, and a **script** to download the full Universal Dependencies release and build a large benchmark-scale index (on the order of **tens of millions of tokens**, depending on UD version and release).

## 1. Bundled CoNLL-U (`test/data/sample.conllu`)

A small [Universal Dependencies](https://universaldependencies.org/) file at [`test/data/sample.conllu`](../test/data/sample.conllu). Use it to verify a local build and try queries without preparing your own data.

### Build the tools

From the **repository root** (requires CMake and a C++17 compiler):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
```

You need at least `build/pando` and `build/pando-index`.

### Build an index from the CoNLL-U file

Indexed data must live in a **directory** (a set of `.dat`, `.lex`, `.rgn`, `corpus.info`, etc.). Pick an output path that is **not** committed to git, for example:

```bash
./build/pando-index test/data/sample.conllu test/data/sample_idx_local
```

- First argument: input `.conllu` file (or a directory of `.conllu` files).
- Second argument: empty or new directory that will hold the index.

If you see errors about missing files when querying, the index may be incomplete or built with an older tool version — **re-run `pando-index`** as above.

Optional: `pando-check <index_dir>` checks consistency of the indexed corpus.

### Run queries

Use the **`pando`** CLI with the **index directory** as the first argument, then the query string:

```bash
./build/pando test/data/sample_idx_local '[lemma="the"]' --limit 5
./build/pando test/data/sample_idx_local '[upos="VERB" & lemma="sit"]'
```

- Default output is KWIC-style concordance lines.
- Useful flags: `--json`, `--limit N`, `--context N`, `--debug`.

#### Experimental CWB front-end

To exercise the `--cql cwb` dialect adapter (subset implementation):

```bash
./build/pando test/data/sample_idx_local --cql cwb --debug '[lemma="the"]'
```

## 2. JSONL fixture (`dev/sample-rich-events.jsonl`)

[`dev/sample-rich-events.jsonl`](../dev/sample-rich-events.jsonl) is a **richer** input in the JSONL event format expected by `pando-index`: multiple region types, nested/overlapping structures, and **multivalue** fields. Use it when you want to test **structural** queries, `nvals`, `::` filters, or multivalue behavior beyond the small UD snippet.

Build an index (example output directory):

```bash
./build/pando-index dev/sample-rich-events.jsonl /tmp/sample-rich-idx
./build/pando /tmp/sample-rich-idx '<your CQL>' --limit 5
```

Details of the event format and `corpus.info` expectations are in [Index and corpus layout](Index-and-Corpus-Layout.md) and related `dev/` notes.

## 3. Full UD release via `scripts/build_ud_corpus.py`

For a **large** corpus spanning the published UD treebanks, use [`scripts/build_ud_corpus.py`](../scripts/build_ud_corpus.py). It:

- Downloads the **latest published** UD treebank archive from LINDAT (or a URL you pass with `--ud-archive-url`),
- Prepends `# newregion` / `# text_*` metadata so tokens resolve `text_lang`, `text_id`, etc. from open text regions,
- Invokes **`pando-index`** on the extracted treebanks.

Example (adjust paths to your machine):

```bash
./scripts/build_ud_corpus.py --data-dir ~/ud-data \
  --pando-index ./build/pando-index --output-index ~/ud-data/pando_idx
```

Or reuse a treebank tree you already extracted:

```bash
./scripts/build_ud_corpus.py --skip-download --data-dir ~/ud/ud-treebanks-v2.17 \
  --pando-index ./build/pando-index --output-index ./idx
```

See the script’s **docstring** at the top of `build_ud_corpus.py` for defaults, overrides, and REST API resolution of the archive.

## Language reference

Query syntax is documented in [PANDO-CQL.md](PANDO-CQL.md). Examples there refer to the CoNLL-U sample where noted.
