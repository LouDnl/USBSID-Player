# reSIDfp build patches

Local changes to the vendored reSIDfp in `lib/residfp`, kept out of `lib/residfp` itself.

CMake copies `lib/residfp` into the build tree (`<build>/residfp-patched/<hash>/`), applies
every `*.patch` in this directory in name order, and compiles that copy. `lib/residfp` is never
compiled directly and never edited by the build. The hash covers the patches and every file
under `lib/residfp`: a changed patch or a re-vendored reSIDfp gets a fresh copy on the following
configure. Old copies stay in the build tree until the build directory is cleaned.

Patch tool: `patch`, or `git apply` when `patch` is missing. Configure stops with an error when
a patch does not apply.

## Patches

| File | What |
|---|---|
| `0001-scope-tap.patch` | Per voice output tap for oscilloscope views. `Voice::output()` caches its value, `SID::setScopeTap()` takes an int16 buffer, and both `SID::clock()` variants store the three voice outputs at every produced sample. Off (nullptr) by default: audio output is unchanged. |

## Re-vendoring reSIDfp

1. Replace `lib/residfp` with the new release.
2. Configure. A patch that no longer applies stops the configure with the tool's output.
3. Refresh the failing patch against the new sources: copy `lib/residfp` twice (`a/`, `b/`),
   edit `b/`, then `diff -ruN a b > NNNN-name.patch` (paths `a/...` and `b/...`, applied
   with `-p1`).
