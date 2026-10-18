# Third-party libraries

*Version française plus bas.*

| Folder | Origin | License | Changes |
|---|---|---|---|
| `dynarmic/` | [Vita3K/dynarmic](https://github.com/Vita3K/dynarmic), `master` branch (October 2026), with its dependencies in `externals/` | 0BSD (`dynarmic/LICENSE.txt`), and each dependency's own | `Jit::GetBlockMap` (map of compiled blocks, for profiling): `src/dynarmic/interface/A32/a32.h`, `src/dynarmic/backend/arm64/a32_interface.cpp`, `src/dynarmic/backend/arm64/address_space.h`, `src/dynarmic/backend/x64/a32_interface.cpp` |
| `boost/` | [Boost 1.92.0](https://github.com/boostorg/boost/releases/tag/boost-1.92.0): only the 763 headers Dynarmic uses (icl, variant, mpl…), copied by `tools/boost_subset.py` | Boost Software License 1.0 (`boost/LICENSE_1_0.txt`) | none |
| `dlmalloc/` | dlmalloc 2.8.6 by Doug Lea | MIT-0 (at the top of `malloc.c`) | none |

After updating Dynarmic or changing the NDK, the list of Boost headers must be regenerated: put a full Boost in place
of `third_party/boost`, build both ABIs (`.\build.ps1` and `.\build.ps1 -Abi x86_64`), move that full Boost to
`third_party/_dl/boost_full`, then run
`python tools/boost_subset.py third_party/_dl/boost_full <the SDK's ninja.exe> build/arm64 build/x86_64`.

## Bibliothèques tierces

| Dossier | Origine | Licence | Modifications |
|---|---|---|---|
| `dynarmic/` | [Vita3K/dynarmic](https://github.com/Vita3K/dynarmic), branche `master` (octobre 2026), avec ses dépendances dans `externals/` | 0BSD (`dynarmic/LICENSE.txt`), et celles de chaque dépendance | `Jit::GetBlockMap` (carte des blocs compilés, pour le profilage) : `src/dynarmic/interface/A32/a32.h`, `src/dynarmic/backend/arm64/a32_interface.cpp`, `src/dynarmic/backend/arm64/address_space.h`, `src/dynarmic/backend/x64/a32_interface.cpp` |
| `boost/` | [Boost 1.92.0](https://github.com/boostorg/boost/releases/tag/boost-1.92.0) : seulement les 763 en-têtes que Dynarmic utilise (icl, variant, mpl…), copiés par `tools/boost_subset.py` | Boost Software License 1.0 (`boost/LICENSE_1_0.txt`) | aucune |
| `dlmalloc/` | dlmalloc 2.8.6 de Doug Lea | MIT-0 (en tête de `malloc.c`) | aucune |

Pour mettre Dynarmic à jour ou changer de NDK, il faut régénérer la liste des en-têtes Boost : mettre un Boost
complet à la place de `third_party/boost`, reconstruire les deux ABI (`.\build.ps1` et `.\build.ps1 -Abi x86_64`),
déplacer ce Boost complet dans `third_party/_dl/boost_full`, puis lancer
`python tools/boost_subset.py third_party/_dl/boost_full <ninja.exe du SDK> build/arm64 build/x86_64`.
