# Sample corpus: build, index, and query

The repository includes a small [Universal Dependencies](https://universaldependencies.org/) file at [`test/data/sample.conllu`](../test/data/sample.conllu). Use it to verify a local build and try queries without preparing your own data.

## 1. Build the tools

From the **repository root** (requires CMake and a C++17 compiler):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
```

You need at least `build/pando` and `build/pando-index`.

## 2. Build an index from the CoNLL-U file

Indexed data must live in a **directory** (a set of `.dat`, `.lex`, `.rgn`, `corpus.info`, etc.). Pick an output path that is **not** committed to git, for example:

```bash
./build/pando-index test/data/sample.conllu test/data/sample_idx_local
```

- First argument: input `.conllu` file (or a directory of `.conllu` files).
- Second argument: empty or new directory that will hold the index.

If you see errors about missing files when querying, the index may be incomplete or built with an older tool version — **re-run `pando-index`** as above.

Optional: `pando-check <index_dir>` checks consistency of the indexed corpus.

## 3. Run queries

Use the **`pando`** CLI with the **index directory** as the first argument, then the query string:

```bash
./build/pando test/data/sample_idx_local '[lemma="the"]' --limit 5
./build/pando test/data/sample_idx_local '[upos="VERB" & lemma="sit"]'
```

- Default output is KWIC-style concordance lines.
- Useful flags: `--json`, `--limit N`, `--context N`, `--debug`.

### Experimental CWB front-end

To exercise the `--cql cwb` dialect adapter (subset implementation):

```bash
./build/pando test/data/sample_idx_local --cql cwb --debug '[lemma="the"]'
```

## 4. Language reference

Query syntax is documented in [PANDO-CQL.md](PANDO-CQL.md). Examples there refer to this sample corpus where noted.
