# vip_btw (Plugify)

[VIP] Buy Team Weapon: registers a 'mm_<name>' console and '!<name>' chat command for every weapon of vip_btw.ini's CT/T sections, letting a VIP with the 'btw' feature buy the opposing team's weapons on their own table (price and buy-zone checks as in the original). Ported from the Pisex VIP core module VIP_BuyTeamWeapon (vip_btw). Requires the vip core.

Plugify port (fork) of **VIP_BuyTeamWeapon ([VIP] Buy Team Weapon)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-vip-modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, vip, hudmenu (optional)

## Installation
Download `vip_btw-1.0.1.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/vip_btw`.

`plugins/vip` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

