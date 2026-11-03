# third_party

Dependencies are pulled with CMake `FetchContent` (pinned to a tag or commit) — nothing is vendored here
unless a patch is unavoidable. **Every dependency must be MIT / BSD / Apache-2.0 / zlib.** No GPL, LGPL,
or non-commercial licenses, anywhere in the dependency tree. See `../THIRD_PARTY.md` for the approved list.

If you need functionality that only exists under a copyleft or non-commercial license, re-implement it
from the paper / spec — never port the code.
