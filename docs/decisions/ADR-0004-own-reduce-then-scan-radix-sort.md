# ADR-0004 — Implement our own reduce-then-scan radix sort

**Status**: accepted (2026-08-31)

**Context**: The per-frame depth sort dominates mobile splat rendering (`docs/sorting.md`).
`jaesung-cs/vulkan_radix_sort` (MIT) is the obvious library, but as of July 2026 it requires Vulkan 1.4
push descriptors, is written in Slang, is tuned for NVIDIA subgroup-32, and explicitly excludes
small-subgroup mobile GPUs. Onesweep/decoupled look-back designs are unsafe on mobile drivers.

**Decision**: Write a GLSL reduce-then-scan LSD radix sort (upsweep / spine / downsweep, 8-bit
digits, key/value, 16- or 32-bit keys, indirect dispatch, no forward-progress or subgroup-size
assumptions). Benchmark it against `vulkan_radix_sort` on the RTX 3060 (reference only; not linked)
and publish desktop and Adreno numbers, including negative results.

**Consequences**: ~300 lines of shader + dispatch code we own; a standalone `sortbench` target and
correctness test are Phase-2 deliverables; sort optimizations require the benchmark to move.
