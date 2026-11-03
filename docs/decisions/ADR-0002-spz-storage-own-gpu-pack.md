# ADR-0002 — SPZ as on-disk format, own SoA GPU pack at runtime, PLY for interop

**Status**: accepted (2026-08-31)

**Context**: PLY is 56–236 B/splat, float-only, no compression, ambiguous coordinate system. We want
something smaller, GPU-friendly and permissively licensed; we must still read the PLYs we already have.

**Options**: (a) keep PLY; (b) SOG (PlayCanvas, MIT, WebP image planes — designed for texture fetch in
their web engine, needs a WebP decoder and is more than we need at SH 0–1); (c) SPZ (Niantic, MIT,
~10× smaller, zstd, explicit coordinate systems, wide adoption); (d) invent a container.

**Decision**: SPZ on disk; PLY import (degree 0–3) and export (degree 0–1); a private in-memory GPU
pack (`docs/formats.md`) built at load. Morton ordering is applied before writing SPZ so it survives.
SOG export is a Phase-5 stretch for web sharing.

**Consequences**: one small decode step at load (parallel zstd, ms-level); dependency on `nianticlabs/spz`
(MIT) + zstd (BSD). Coordinate-system conversion is centralized in `splatcore`.
