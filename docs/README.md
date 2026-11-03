# docs/ — documentation map and update contract

This file is the contract for where information lives and which document changes when something
happens. It exists so the docs stay trustworthy without anyone having to remember the rules.
If a change doesn't fit any row of the contract table, that is a smell — discuss before inventing
a new document.

## Document map

| Document | Contains | Audience |
|---|---|---|
| `../README.md` | Public pitch, hero media, headline numbers, try-it, pipeline, measured results (figures), engineering notes, known limitations, build instructions, scene credits | Public |
| `../CLAUDE.md` | Hard constraints, toolchain, conventions, build/test commands, workflow rules | Contributors / AI sessions |
| `../THIRD_PARTY.md` | Every dependency: pinned tag, license, verified LICENSE first line | Contributors |
| `../PROJECT.md` *(gitignored)* | Private roadmap: strategy, phase plan with gates, risk register, decision log, session log | Owner only — never committed |
| `formats.md` | PLY/SPZ/GPU-pack layouts, encodings, coordinate systems | Contributors |
| `cleaning.md` | Cleaning ops, defaults, ordering, reporting rules | Contributors |
| `sorting.md` | Why the mobile sort is hard, our sort design, rejected alternatives | Contributors |
| `coverage-mask-research.md` | Per-splat observed-direction mask from the capture cameras: prototype, hole policies, why the hole is inherent | Contributors |
| `phase0-webgpu-baseline.md` | Phase 0 measurement protocol (WebGPU baseline on the S24U) | Contributors |
| `decisions/ADR-NNNN-*.md` | One accepted architectural decision each: context, options, decision, consequences | Contributors |
| `benchmarks/README.md` | Benchmark file index, CSV column schema, measurement protocol, **negative results log** | Contributors + public evidence |
| `benchmarks/*.csv`, `benchmarks/*.md` | Curated measurements (device, build hash, scene, per-pass timings, thermal) and cleaning reports | Public evidence |
| `figures/*-light.svg`, `figures/*-dark.svg` | README figures, regenerated from the committed CSVs by `scripts/plot_readme.py` (never edited by hand) | Public evidence |
| `media/*` | README media (turntables of open-licence scenes), made by `scripts/make_turntable.py`; phone screen recordings | Public |
| `../data/README.md` + `../data/manifest.json` | Test-set sources and licenses; the data itself is never committed | Contributors |

## Update contract — when X happens, update Y

| When you... | Then update |
|---|---|
| Make or change an architectural decision | New ADR in `decisions/` (next number). ADRs are immutable once accepted: supersede with a new ADR that links back, never edit history |
| Add, bump or drop a dependency | `THIRD_PARTY.md` row (tag + license + LICENSE first line), and the license check per ADR-0005 |
| Measure anything (fps, ms, sizes, quality) | CSV or table under `benchmarks/` with device, build hash, scene, methodology; a summary sentence in `benchmarks/README.md`. Numbers are never quoted from memory |
| Try something that turns out slower or worse | Row in the negative results log in `benchmarks/README.md`: what, measurement, diagnosis. Negative results are first-class output |
| Change a format, layout or encoding | `formats.md`, plus an ADR if it reverses a recorded decision, plus the golden-image/byte-exact tests that gate it |
| Change cleaning defaults or thresholds | `cleaning.md` + an ADR or documented benchmark justifying it (never silently) |
| Add a test scene | `../data/manifest.json` entry with source, license, sha256 |
| Finish a work session | `PROJECT.md` session log row (private): date, phase, what landed, what's next |
| Pass a phase gate | `PROJECT.md` phase status; when results become public-worthy, move numbers into `../README.md` |
| Learn a hard-won environment/toolchain fact | `../CLAUDE.md` (so no future session rediscovers it) |

## Rules

* **No CHANGELOG file** — history is git log plus the private session log.
* **Public docs carry evidence, not promises**: the public `README.md` gets performance numbers only
  once they exist as committed CSVs under `benchmarks/`.
* **One topic, one home**: formats live in `formats.md`, not scattered in code comments; the code
  comment points at the doc section, not the other way round.
* **ADR numbering** is monotonic (`ADR-0015` is next); status is `proposed` (design written and
  measured, not yet implemented or reviewed — may still change), `accepted`, or `superseded by
  ADR-NNNN`. Small reversible choices don't need an ADR — anything that would surprise a
  contributor six months later does.
* Never store absolute local paths, device serials, or anything personal in committed docs.
