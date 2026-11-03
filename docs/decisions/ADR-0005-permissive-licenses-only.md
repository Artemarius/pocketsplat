# ADR-0005 — Permissive licenses only; re-implement instead of depending

**Status**: accepted (2026-08-31)

**Decision**: The project is MIT. Every dependency, transitive included, must be MIT, BSD, Apache-2.0
or zlib. GPL/LGPL/MPL/CC-NC/"research only" code is never linked, vendored, or ported — including the
INRIA 3DGS reference, `diff-gaussian-rasterization`, 3DGS.cpp (LGPL), and unreleased research code.
Such repositories may be *read* to understand an algorithm; the implementation here is written from the
paper/spec and says so in a comment. Test data with non-commercial licenses stays local and unshared.

**Consequences**: Occasionally more work (e.g. the sort, kNN grid, PLY IO), which is also the point of a
portfolio project. `THIRD_PARTY.md` is the single source of truth and is checked on every new dependency.
