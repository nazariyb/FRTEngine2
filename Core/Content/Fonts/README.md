# Fonts

| File | Source | License |
|------|--------|---------|
| `Roboto-Medium.ttf` | Copied from `ThirdParty/Imgui/misc/fonts/` | Apache License 2.0 |

`Roboto-Medium.ttf` is the editor UI font, loaded in `GameInstance::GameInstance()`.

Kept here rather than referenced in place so the runtime does not depend on the layout of
the Dear ImGui submodule. If the file is missing, ImGui falls back to its built-in font.
