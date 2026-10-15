# Reconciled beta: manual acceptance record

Back up the tester's active Citron data directory. Record the executable
hash, backend version, firmware, OS/GPU, game/version and controller. Do not attach
keys, account tokens, saves or unredacted logs to a public issue.

The original beta54.3 and the main-reconciled candidate are different binaries.
Historical owner confirmations do not automatically pass the candidate below.

| Test | Procedure / expected result | Reconciled candidate |
| --- | --- | --- |
| Startup | Start normally; completed wizard is not forced again | Pending |
| HOME boot | Start with --home-menu; unlock; navigate using mouse and keyboard | PASS: --home-menu boot and mouse control selection |
| Homebrew round trip | Open Ctrl+H, return Ctrl+H; repeat three times; HOME remains responsive | PASS: three consecutive Ctrl+H cycles, child cleanup verified |
| Controller return | HOME and L+R+Minus from hbmenu; no spontaneous reopen | Pending physical controller |
| Tools available | hbmenu lists the four configured tools; no wizard requirement | PASS: all four tools visible without wizard prompt |
| Settings applet | Open/close System Settings; return to responsive HOME | Pending |
| Profile | Open/close native profile; check friend code/history/member label | Pending account/manual |
| Photo editing | Open native avatar editor and return | Known unresolved; do not mark fixed |
| New content | Copy one complete base game to a configured folder; wait for copy completion | Pending |
| Update/DLC | Add matching external content recursively; reopen Software Information; verify version and removal | Pending |
| Slow copy | Add large update slowly; no parse/crash before completed copy | Pending |
| Themezer/NXThemes | Mouse navigation, preview, download, install and reboot | Pending |
| Connection data | Compare actual adapter type/band/channel/IP/DNS; no inferred bandwidth | Pending host test |
| Connection test | Complete download/upload, return, reopen status; last NAT persists and retests on changed network | Pending live network |
| Suspended game | Splatoon 3 -> HOME for >5 seconds -> resume/switch | Known unresolved freeze/crash |
| BCAT lifecycle | Matching package accepted; incomplete/failing update keeps previous cache; refresh before boot | Pending game/backend acceptance |

A pass for basic HOME/homebrew navigation is not acceptance of a suspended-game
lifetime fix. Keep the PR draft until maintainers approve its remaining scope.

Tested Windows candidate SHA-256:
`76b8e6dedce3664d3202ead0ec6eb3ec0d73e6fd08972b9729b4ffe40578cfe3`.
Normal desktop shutdown also completed. No game was launched in this smoke run.

## Normal Windows distribution

Beta54.3.1 and later omit portable data and use the existing Windows path manager:
without a deliberate `user` folder they share `%APPDATA%\citron`. The additional
setup-startup test covers legacy configuration and launches from two executable
version directories. The Windows focused suite passed 10/10; Linux normal and
ASan/UBSan also passed [run 38702831600](https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38702831600).
The owner deferred the normal-startup manual check, which is still pending.
The desktop smoke recorded above applies to the earlier reconciled binary only.
