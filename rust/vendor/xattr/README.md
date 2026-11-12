# xattr compatibility patch

This is `xattr` 0.2.3 from crates.io, vendored because the GPUI 0.2.2 dependency chain currently selects it and its Linux implementation references `libc::ENOATTR`, which is not defined for Linux targets. Linux and Android report missing extended attributes as `ENODATA`; `src/sys/mod.rs` maps the compatibility constant to that errno on those targets. Other platform mappings are unchanged.

The crate's upstream license files are included. Remove this patch once the upstream dependency graph no longer requires it.
