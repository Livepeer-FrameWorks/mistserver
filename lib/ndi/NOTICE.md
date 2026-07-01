# Vendored NDI SDK headers

The headers under `include/` are the public interface headers from the **NDI SDK
v6.3.0.3** (NDI 2026-01-21), copyright © 2023-2026 Vizrt NDI AB. They were obtained
from the [DistroAV](https://github.com/DistroAV/DistroAV/tree/master/lib/ndi) MIT
mirror (the same headers ship in the official SDK) and normalized to LF line endings.

Each header carries its own per-file **MIT license** notice (see the comment block
at the top of every `Processing.NDI.*.h` file). That MIT grant applies to these
header files **only** — not to the NDI SDK as a whole. Per the NDI SDK
documentation, open-source projects are explicitly permitted to redistribute these
headers under MIT for use with **dynamic loading** of the NDI runtime libraries.

## Why these are vendored

MistServer does **not** link against the NDI SDK at build time. NDI support is
compiled from these headers alone and the runtime library (`libndi.so.6` /
`libndi.dylib`) is loaded at runtime via `dlopen()` + `NDIlib_v5_load()` (see
`lib/device_ndi.cpp`). For our targets (**Linux and macOS server**) the SDK provides
**no static library** — only the dynamic `libndi.so.6` / `libndi.dylib`; static
archives ship only for iOS/tvOS, which we do not target. So dynamic loading is the
only viable integration path, and vendoring the headers is what lets the build
produce a clean binary with no link-time `libndi` dependency.

The NDI runtime itself is **not** bundled. Users who want NDI must install the NDI
runtime/redistributable (<http://ndi.link/NDIRedistV6>); when it is absent NDI
support is simply a runtime no-op.

## Updating

To update, replace the contents of `include/` with the headers from a newer NDI SDK
`include/` directory (or the DistroAV mirror), **normalize them to LF** (`tr -d '\r'`),
and update the version line above. Keep the per-file MIT notices intact. The
`.gitattributes` entry for `lib/ndi/**` keeps them LF and exempt from whitespace checks.
