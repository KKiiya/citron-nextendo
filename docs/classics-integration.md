# Classics integration and remaining client issues

The backend handoff is
[online-classics PR 5](https://github.com/NextendoNetwork/online-classics/pull/5).
It includes six Go modules, signed-account verification and bounded relay/session
lifecycle. Updating the server does not install title-specific client patches.

Historical Citron tests used baseline
`38e110fa46c1b6c5822f5b15db95852bbfd8b278` and a local audio diagnostic build.
The GBA/SNES client integration introduced no additional core repair. The
diagnostics observed Genesis audio/suspension stalls; they did not repair them.
This handoff adds no speculative audio, qlaunch or guest-memory workaround.

| Catalogue | Version | Historical operator report / limit |
| --- | --- | --- |
| NES | 9.1.0 | Citron/Ryujinx gameplay; repeat against signed-account service |
| SNES | 6.0.0 | Citron/Ryujinx gameplay; repeat lifecycle and public-network tests |
| Game Boy | 4.1.0 | Citron/Ryujinx gameplay; base 1.0.0 is not the tested setup |
| GBA | 3.3.0 | Citron/Ryujinx gameplay and both host directions |
| Genesis | 3.1.1 | Suspension freezing remains unresolved |
| N64 | 4.2.0 | Citron/Switch gameplay reported; duration and reverse-host coverage unrecorded |

These reports do not certify all games in a catalogue or the current upstream
client. Record exact emulator/server commits, installed title version, Build ID,
authorized patch version, host roles and topology. Use maintainer-confirmed routing
and distinct authenticated accounts. Keep raw logs and account material private.

## Acceptance before rollout

1. Verify the expected signed account and real friend identity reach the backend.
2. Create, discover and join a room in both host directions.
3. Start a game and verify input from both players; record game and duration.
4. Suspend/resume, return to the catalogue, change game, leave/rejoin and recreate.
5. Repeat on physical Switch, separate networks and four players as applicable.
6. Investigate Genesis suspension separately. Existing local audio diagnostics
   are evidence gathering, not a release fix. Do not suppress failures or claim
   completion based on room discovery alone.

Current friend/image handling is already in upstream. The separately submitted
Ryujinx cold-cache and Windows callback changes do not constitute a new Citron
fix. Firmware, NAND, keys, game archives, IPS files and experimental qlaunch
changes are outside this server/client integration handoff.
