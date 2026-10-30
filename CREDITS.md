# Credits and acknowledgements

## The game

Greenscreen '89 plays the game only from a copy its owner supplies. The game belongs to its rights holders.

| Who | Role |
| --- | --- |
| Alexey Pajitnov | Created Tetris (1984–85); original concept, design and program |
| Tetris Holding, LLC / The Tetris Company | Own and license the Tetris rights, trademark and trade dress; founded 1996 by Pajitnov and Henk Rogers |
| ELORG (Elektronorgtechnica) | Held and licensed the Tetris rights in 1989; credited on the cartridge's startup screen |
| Nintendo (Nintendo R&D1) | Developed and published Tetris for Game Boy (1989); owns the Game Boy trademarks |
| Bullet-Proof Software (Henk Rogers) | Secured the handheld rights and sub-licensed them to Nintendo |
| Satoru Okada | Director |
| Gunpei Yokoi | Producer |
| Masao Yamamoto | Programmer and game designer |
| Hirofumi Matsuoka | Graphics |
| Hirokazu Tanaka | Composer and arranger |

Sources: [Wikipedia](https://en.wikipedia.org/wiki/Tetris_(Game_Boy_video_game)), [The Tetris Company on Wikipedia](https://en.wikipedia.org/wiki/The_Tetris_Company), [MobyGames](https://www.mobygames.com/game/42121/tetris/credits/gameboy/).

## Research this port builds on

- [tetris-gb-disasm](https://github.com/vinheim3/tetris-gb-disasm) by vinheim3 (Daniel Jianoran), MIT License: the names and meanings of the game's RAM variables, states and link routines.
- [Pan Docs](https://gbdev.io/pandocs/) and the [gbdev](https://gbdev.io) community: Game Boy hardware reference.
- [SameBoy](https://github.com/LIJI32/SameBoy) and [Peanut-GB](https://github.com/deltabeard/Peanut-GB): open-source emulators kept on hand as references. None of their code is used here.

## Tools and platform

- [LLVM / Clang / LLD](https://llvm.org) (Apache 2.0 with LLVM exceptions)
- Android NDK headers from [AOSP](https://source.android.com) (Apache 2.0)
- [Python 3](https://www.python.org) (PSF License)
- [OpenJDK](https://openjdk.org) `keytool` / `jarsigner` (GPL v2 with Classpath Exception)

## Online play

Matchmaking uses free public MQTT brokers: [test.mosquitto.org](https://test.mosquitto.org/) (the [Eclipse Mosquitto](https://mosquitto.org) project), [broker.emqx.io](https://www.emqx.com/en/mqtt/public-mqtt5-broker) (EMQX) and broker.hivemq.com (HiveMQ). STUN uses stun.l.google.com and stun.nextcloud.com. These are public test services: lobby messages carry only a random ID and network addresses, and anyone can read them.

## Notices

Tetris ® & © Tetris Holding. Tetris trademarks and trade dress are licensed by The Tetris Company. Game Boy is a trademark of Nintendo. Greenscreen '89 is an unofficial fan project, not affiliated with or endorsed by any of the companies above. No ROM is included or distributed. Personal, non-commercial use only. This is a good-faith credits list, not legal advice.
