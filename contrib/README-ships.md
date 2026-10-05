# Third-party code for the custom ships

Used by the ship converter `common/tools/shipconv.cpp` (and, for PNG
decoding, by the game's ship loader). Each file is vendored unchanged except
for a first line `#pragma GCC system_header`, which keeps the game's strict
warning flags (`-Werror`, `-Wold-style-cast`, …) out of third-party code.

| File | Version | Licence | Source |
|---|---|---|---|
| `cgltf/cgltf.h` | 1.14 | MIT (text at the end of the file) | https://github.com/jkuhlmann/cgltf |
| `stb/stb_image.h` | 2.30 | public domain or MIT (end of the file) | https://github.com/nothings/stb |
| `stb/stb_image_write.h` | 1.16 | public domain or MIT (end of the file) | https://github.com/nothings/stb |
