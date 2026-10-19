# JSON parser

`json.hpp` is nlohmann/json 3.12.0 (MIT), copied from the llama.cpp vendor tree
at commit `797de63b2fa7396dcc8caca76834c699ac84e1c3`.

Upstream: https://github.com/nlohmann/json/tree/v3.12.0

This copy includes llama.cpp's deep-nesting backports, recorded in the adjacent
`json-deep-nesting.patch`. The patch is already applied; do not apply it again.
The slot adapter separately limits JSON response size, nesting depth, and slot
count. The library is bundled so building btop needs no new system package or
network fetch. Its license is installed to `share/btop/licenses/LICENSE-jsonhpp`.
