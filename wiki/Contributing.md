# Contributing

## Layout

- **`src/`** — C++ core: `corpus/`, `index/`, `query/`, `api/`, `cli/`
- **`wiki/`** — User-facing documentation (overview, [PANDO-CQL.md](PANDO-CQL.md), [Sample-Corpora.md](Sample-Corpora.md), topic pages)
- **`docs/`** — [`README.md`](../docs/README.md) redirects to the wiki for anyone following old paths
- **`dev/`** — Design notes, benchmarks, roadmaps (not shipped)

## Design notes

See `README.md` in the repo root for pointers to `dev/ROADMAP-TODO.md`, `dev/PANDO-INDEX-INTEGRATION.md`, `dev/QUERY-COMPAT.md`, etc.

## Wiki

- Prefer updating **topic wikis** for short, stable explanations and **PANDO-CQL.md** for full language tutorial.
- Preview Markdown locally: `python scripts/serve-wiki-preview.py`

## Build

See [Installation](Installation.md).
