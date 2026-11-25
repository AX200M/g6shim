# xgameruntime

## add Silent stubs

- Speech synthesis answers success with a silent 44-byte WAVE file (16 kHz mono 16-bit PCM, no samples) for every text and SSML stream; voice enumeration lists none and never calls back.
  Closes when: speech goes through the host's synthesis engine.

## add Functional

- HDR is reported disabled (`2`) because presentation is SDR; the host display's luminance is not read.
  Closes when: the swap chain can present HDR.
- Screenshots are refused with `0x80070032`; taking one needs the last presented frame read back as BGRA8.
- URIs are not launched: there is no store, browser or console app here, so the call fails with `0x80070032`.
  Closes when: a URI handler maps to a host application.
- Every chunk selector counts as installed, including selectors the package layout does not have; the package's byte counts are measured once and installation progress never changes.
- Installing chunks reports success and hands out a finished monitor without copying anything; uninstalling chunks is accepted and removes nothing, because the package folder belongs to the user.
- Package enumeration lists no content packages, the game kind included.
- The game save provider keeps no cloud copy: `needs_sync` is always false, nothing is uploaded and saves sync only as files on disk.
- A profile card completes at once with nothing shown; message, achievement, player-picker and error dialogs stay unimplemented.
