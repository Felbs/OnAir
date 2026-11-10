# External engine

A television receiver that runs in another program. OnAir starts it, tells it the channel, plays its
transport stream with the usual player and controls, and shows what the receiver measured. The mode
is in the TV group as **External engine** (family 241). No radio is opened by OnAir: the engine owns
its own.

The first engine is **tvTuna** (`tvtuna engine`): ATSC 1.0 through the Software TV Tuner's 8-VSB
chain and ATSC 3.0 through the atsc3 receiver, both behind gr-rxtune's gain search and servo, judged
on decoder output (the equaliser's training error AND pictures arriving, or the share of FEC blocks
that come out clean), and behind a site radio warden that shares one SDR between many programs.

## Try it first with no radio

The engine can play a recorded transport stream in place of a receiver, so the mode, the pipes, the
socket and the player can be checked on any computer:

1. Get the engine: [github.com/Felbs/tvTuna](https://github.com/Felbs/tvTuna) (GPL-3.0-or-later),
   then `pip install .` in the clone. It needs Python 3.9 or later and nothing else for this step.
2. Take any MPEG-TS recording, or make a 20-second test pattern with the ffmpeg line in tvTuna's
   README ("Try it without a radio").
3. In OnAir pick **External engine**, open **Engine**, set the engine command to
   `<full path to that python> -m tvtuna engine --tcp 8555 --replay <full path to the file.ts>`,
   give any RF channel number, press **Start and tune**.

The picture plays in a loop and the line above it reads "decoding". The measurement row shows the
packet and continuity counts of the bytes sent; the servo line reads `replay (no radio)` and there
is no MER, because nothing is being received. Checked on 2026-11-09 (Windows 11, tvTuna 65ce10e
installed from a fresh clone): 1344 pictures shown, 0 late, 0 decode errors, 0 continuity errors
across two passes of the pattern.

## Using it on real air

1. Set the engine up as tvTuna's README says ("Setting up for real air": an interpreter with
   SoapySDR and GNU Radio, the Software TV Tuner, gr-rxtune, the atsc3 receiver if ATSC 3.0 is
   wanted, then `tvtuna init ...`). `tvtuna doctor` checks every piece and must end with
   `0 failure(s)`. Get a first picture with `tvtuna watch --rf N` before bringing OnAir in.
   Tested hardware: one SDRplay RSPdx on Windows 11. The gain search turns the RSP's two gain
   knobs; other radios are not written yet.
2. In OnAir pick **External engine**, check the engine command, the TS host and port, give the RF
   channel number (or click one from the engine's own table once the engine has started), press
   **Start and tune**. The default command `python -m tvtuna engine --tcp 8555` only works when
   the `python` OnAir finds first is the one tvTuna was installed in; otherwise give that
   interpreter's full path. A channel the engine has not seen before is taken as ATSC 1.0 and gets
   a full gain search (about three minutes before the first hold); an ATSC 3.0 channel must be
   tuned once from the command line first (`tvtuna tune --rf N`).
3. The picture appears in the mode's tab with the usual player controls (volume, subtitles, audio
   track, full screen, pop out, recording). The tab keeps one row of controls above it; the engine's
   command, host and port fold away under **Engine** while a channel plays. The main programme of the
   multiplex is played: the lowest programme number, not whichever table arrived first.
4. The row under the tabs, where the TV modes have their constellations, shows what the engine
   measured: the servo state and the held gain setting, the MER against the 15.2 dB data cliff,
   pictures per second (ATSC 1.0) or frames and interval FEC (ATSC 3.0), packet and
   continuity-error counts, and who holds the radio.

Leaving the screen stops the engine; so does closing OnAir. The engine is always asked to stop
(it must close its radio itself) and is only killed if it has not exited after 90 s.

## The protocol

One JSON object per line. OnAir writes to the engine's stdin and reads its stdout; the transport
stream comes over a local TCP socket the engine listens on.

| direction | line |
|---|---|
| to the engine | `{"cmd":"tune","rf":34}` `{"cmd":"status"}` `{"cmd":"stop"}` `{"cmd":"quit"}` |
| from the engine | `{"ev":"hello","tcp":8555,"channels":{"34":{...},"33":{...}},"radio":null}` once at start |
| | `{"ev":"up","rf":34,"standard":"atsc1","tcp":8555}` when a stream file exists for the channel |
| | `{"ev":"status","rf":34,"standard":"atsc1","up":true,"radio":{"owner":"tvtuna","purpose":"live TV"},"last":{...}}` every 2 s while tuned |
| | `{"ev":"error","rf":36,"msg":"chain did not start"}` `{"ev":"released","rf":34}` |

`last` carries the engine's decoder truth: for ATSC 1.0 `state` (searching / holding / probing),
`setting` (`{"gain:RFGR":4,"gain:IFGR":39}`), `mer_db`, `video`, `pictures_per_s`, `packets`,
`cc_errors`, `nulls`, `tei`; for ATSC 3.0 `frames`, `fec_pct`, `sink_bytes`. Unknown values are
`null`. Any other program that speaks these lines is an engine too.

The byte stream on the socket is MPEG-TS, 188-byte packets, with null packets while the receiver is
quiet so the demuxer never starves. OnAir realigns it (`iptv::TsAligner`) and pushes it to the
Player the way the Internet TV mode does.

## Where it lives

`core/src/extengine/` (the process, pipes, status reader and TS reader; `extengine_mode.cpp` is the
mode row with a silent test signal), `app/extengine/extengine_ui.cpp` (the screen), `tests/extengine/`
(the status lines fold into the fields the panels read; lines recorded from tvTuna on real air),
`tests/app/test_extengine_mode.cpp` (numbers, row, screen mapping, feed branch).

## Why

Measured on real air with tvTuna's receivers (2026-11-09): the gain servo's probe-and-revert and
re-search on decoder output, the fade-versus-wedge call from the receiver's own frames and interval
FEC, and one radio shared by several programs through a warden. Those are the parts of live TV a
player cannot see from the picture alone, and this mode puts them next to it.
