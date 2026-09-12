# Host renderer fonts

`host_fonts.h` contains unmodified compressed font data extracted from
`libu8g2.a` (`u8g2_fonts.o`) in the official Flipper Zero 1.4.3 / API 87.1 SDK.
The arrays are used only by host tests and screenshot generation. Their SHA-256
hashes are recorded beside each array. The Flipper build continues to use its
firmware fonts.

The font selection matches the [firmware Canvas implementation](https://github.com/flipperdevices/flipperzero-firmware/blob/1.4.3/applications/services/gui/canvas.c).
The byte tables are `u8g2_font_helvB08_tr` (primary) and
`u8g2_font_haxrcorp4089_tr` (secondary). To refresh them, extract the corresponding
`.rodata.u8g2_font_*` sections from the official SDK with `arm-none-eabi-objcopy`.
Preserve these notices and update the recorded hashes when changing the data.

## HaxrCorp 4089

“HaxrCorp 4089” by **sahwar** is licensed under
[Creative Commons Attribution-ShareAlike 3.0](https://creativecommons.org/licenses/by-sa/3.0/).
The font data here retains that license; no glyphs were modified.

- [Original FontStruction](https://fontstruct.com/fontstructions/show/192981/haxrcorp_4089)
- [u8g2 attribution and license reference](https://github.com/olikraus/u8g2/wiki/fntgrpfontstruct#haxrcorp-4089)

## Helvetica Bold 08 (X11 HELVB08)

The following notice is reproduced from [u8g2's font license](https://github.com/olikraus/u8g2/blob/master/LICENSE).

Copyright 1984-1989, 1994 Adobe Systems Incorporated.
Copyright 1988, 1994 Digital Equipment Corporation.

Adobe is a trademark of Adobe Systems Incorporated which may be
registered in certain jurisdictions.
Permission to use these trademarks is hereby granted only in
association with the images described in this file.
Permission to use, copy, modify, distribute and sell this software
and its documentation for any purpose and without fee is hereby
granted, provided that the above copyright notices appear in all
copies and that both those copyright notices and this permission
notice appear in supporting documentation, and that the names of
Adobe Systems and Digital Equipment Corporation not be used in
advertising or publicity pertaining to distribution of the software
without specific, written prior permission. Adobe Systems and
Digital Equipment Corporation make no representations about the
suitability of this software for any purpose. It is provided "as
is" without express or implied warranty.
