# Drum kits

The engine plays SFZ drum kits with [sfizz](https://sfz.tools/sfizz/). The kits are not in git
(`kits/*/` is ignored); download them with:

```sh
kits/fetch-kits.sh            # all kits (~85 MB)
kits/fetch-kits.sh avl-black-pearl
kits/fetch-kits.sh --force    # re-download
```

The script is reproducible: the AVL zips are verified against pinned SHA-256 sums and Sam's Sonor
is fetched at a pinned git commit. For every kit it writes `kits/<kit>/kit.json` and a small
`drums-app.sfz` wrapper next to the kit's own `.sfz`.

| folder | kit | license | default |
|---|---|---|---|
| `avl-black-pearl/` | AVL Drumkits **Black Pearl** (Pearl 22" kick, 14" snare, 12" tom, 16" floor tom, Sabian/Zildjian cymbals), 5 velocity layers | CC-BY-SA 3.0 | yes |
| `avl-red-zeppelin/` | AVL Drumkits **Red Zeppelin** (Ludwig 26" kick, 14" snare, 14" tom, 16" floor tom, Zildjian cymbals), 5 velocity layers | CC-BY-SA 3.0 | |
| `sams-sonor/` | **Sam's Sonor** (Sonor Force 3001), 5–9 velocity layers, 24-bit FLAC | CC-BY-SA 4.0 | |

## Attribution

- **AVL Drumkits** by Glen MacArthur (AV Linux), <https://www.bandshed.net/avldrumkits/>,
  downloaded from <https://www.bandshed.net/sounds/AVLDrumkits_SFZ/>
  (`BLACK_PEARL_2023_repack.zip`, `RED_ZEPPELIN_2023_repack.zip`). Licensed under
  [CC-BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/). Per the author, music made with
  the kits needs no attribution; modified/redistributed sample libraries do.
- **Sam's Sonor** samples by Sam Greene
  (<https://web.archive.org/web/20120224092843/http://www.samgreene.com/sams-sonor-samples>), SFZ
  mapping by kinwie, <https://github.com/sfzinstruments/SamsSonor> (commit `ef3e320`). Licensed
  under [CC-BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) (see `sams-sonor/LICENSE`).

## Modifications (`drums-app.sfz`)

The original kit files are not changed. Each kit has only two real toms, so the generated
`drums-app.sfz` `#include`s the original `.sfz` and adds a **derived mid tom on key 102**: the high
tom's samples tuned 3 semitones down (`tune=-300`). This wrapper is a derivative of the kit and is
under the kit's license (CC-BY-SA, ShareAlike).

At load time the engine also rewrites the SFZ **in memory** (never on disk) to give every
instrument its own sfizz output (`output=` opcode) for the per-instrument mixer, and, for kits
under 300 MB, adds `hint_ram_based=1` so all samples stay in RAM.

## kit.json

```json
{
  "name": "AVL Black Pearl",
  "sfz": "BLACK_PEARL_2023_repack/drums-app.sfz",
  "notes": { "kick": 36, "snare": 38, "snareRim": 37, "hihatClosed": 42, "hihatOpen": 46,
             "hihatPedal": 44, "tom1": 45, "tom2": 102, "tom3": 41, "crash": 49, "ride": 51,
             "rideBell": 53 },
  "hihatCC": 4,
  "license": "CC-BY-SA-3.0",
  "source": "https://www.bandshed.net/sounds/AVLDrumkits_SFZ/BLACK_PEARL_2023_repack.zip"
}
```

- `sfz`: path relative to the kit folder.
- `notes`: instrument → note **in this kit's SFZ** (not the e-drum's notes; those are in
  `kits/pad-map.json`). Instruments the kit lacks are simply left out.
- `hihatCC` (optional, default 4): the CC the kit's SFZ uses for hi-hat openness; the e-drum's
  pedal CC (`hihatPedalCC` in `pad-map.json`) is forwarded to it. None of the three kits responds to it.

| instrument | AVL kits | Sam's Sonor |
|---|---|---|
| kick | 36 | 36 |
| snare / snareRim | 38 / 37 (side stick) | 38 / 37 (rimshot) |
| hihatClosed / Open / Pedal | 42 / 46 (semi-open) / 44 | 42 / 46 / 44 |
| tom1 / tom2 / tom3 | 45 / 102 (derived) / 41 (floor) | 45 / 102 (derived) / 43 |
| crash / ride / rideBell | 49 / 51 / 53 | 49 / 51 / 53 |

To add another kit, put it in `kits/<name>/` with a `kit.json`; the engine lists every
`kits/*/kit.json` in the UI's kit menu.
