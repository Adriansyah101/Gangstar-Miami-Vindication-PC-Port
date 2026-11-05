Decoders compiled into the host for the custom radio (shims/songdec.c, shims/songdec_impl.c).
Unchanged copies, each a single file, kept in the source so that a checkout builds with nothing to fetch.

File          Version  From                                                       SHA-256
dr_mp3.h      0.7.4    github.com/mackron/dr_libs 35b5c392cc15456736c440a68dbe073178bbffe0  F5C8020EEEBB79877184432EFDD2651F24869F5BB8F379D45450FD0AED1EE915
dr_flac.h     0.13.4   github.com/mackron/dr_libs 35b5c392cc15456736c440a68dbe073178bbffe0  B8078EEB5D523502EB7964508CD7EC8450BC79D383BEC90AA17FD9E2FEFD9C24
dr_wav.h      0.14.6   github.com/mackron/dr_libs 35b5c392cc15456736c440a68dbe073178bbffe0  FAD60F1E360D6DE726EFC1B9A7985610C89C551719E3F110E8B992F0E1F308CD
stb_vorbis.c  1.22     github.com/nothings/stb    d14d46f6b44456eb88beac58cba833a4135d4c89  4C7CB2FF1F7011E9D67950446B7EB9CA044F2E464D76BFBB0B84DD2E23E65636

Licences: each file offers a choice of public domain (the Unlicense) or MIT No Attribution (dr_libs) / MIT (stb);
the text is at the end of each file.

The build reads this folder as a system include folder, so the compiler's warnings about these files are not shown
(the host's own code is built with -Wall -Wextra and has none).
