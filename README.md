# Nubby GBA

![Nubby GBA title screen](docs/title-screen.png)

A pegboard number-crunching game for the Game Boy Advance, loosely inspired by
*Nubby's Number Factory*. Drop Nubby into the factory's pegboard, rack up
points and multipliers, and hit each round's goal before you run out of drops.

![A round in progress](docs/board.png)

> **Made with AI:** this game was built with [Claude](https://claude.ai), Anthropic's
> AI model, using Claude Code. The code, artwork, music and documentation were
> written by Claude, directed and playtested by [@hjcoggan](https://github.com/hjcoggan).
> See [AI disclosure](#ai-disclosure) below.

## How to play

- Move Nubby along the top with **Left / Right** (L / R for fine aim) and press **A** to drop.
- Every peg Nubby touches counts once per drop:
  - **White peg**: +1 point
  - **Purple peg**: +1 multiplier
  - **Gold peg**: +1 coin (and +1 point)
  - **Blue bumper**: bounces Nubby hard, +1 point every bounce
- Nubby lands in a bucket (x2, x1, x3, x1, x2). A drop scores
  **points x multiplier x bucket**.
- Reach the round's goal within 5 drops. Goals rise 40% each round; miss one
  and the run is over.
- Clearing a round pays 3 coins plus 1 for each unused drop. Spend them in the
  shop on up to 4 items:

| Item | Effect | Cost |
| --- | --- | --- |
| Heavy | White pegs are worth 2 | 6 |
| Multi | 2 more purple pegs on every board | 6 |
| Extra | 1 more drop each round | 7 |
| Spring | 2 more bumpers, and bumpers are worth 3 | 5 |
| Lucky | Every 7th peg in a drop gives +7 | 5 |
| Piggy | Earn 1 coin for every 4 you have saved | 4 |
| Boost | Each drop starts with 5 points | 5 |
| Bucket | The middle bucket is x5 | 6 |

Your furthest round and best score are saved to cartridge SRAM (a `.sav` file
in emulators).

## Building from source

You need [devkitPro](https://devkitpro.org)'s GBA toolchain, `make` and `git`.
Python 3 is only needed if you change the artwork (`make assets`).

### macOS

1. Download and run the devkitPro pacman installer (`.pkg`) from
   <https://github.com/devkitPro/pacman/releases>, then install the GBA tools:

   ```bash
   sudo dkp-pacman -S gba-dev
   ```

2. Add the toolchain to your shell (append to `~/.zshrc`, then `source ~/.zshrc`):

   ```bash
   export DEVKITPRO=/opt/devkitpro
   export DEVKITARM=$DEVKITPRO/devkitARM
   export PATH=$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH
   ```

3. Install mGBA: `brew install --cask mgba`

4. Build and run:

   ```bash
   git clone https://github.com/hjcoggan/nubby-gba.git ~/nubby-gba
   cd ~/nubby-gba
   make
   open -a mGBA nubby-gba.gba
   ```

### Windows

1. Download the graphical installer (`devkitProUpdater`) from
   <https://github.com/devkitPro/installer/releases> and run it. When it asks
   which components to install, tick **GBA Development**. It installs to
   `C:\devkitPro` and sets the `DEVKITPRO`/`DEVKITARM` variables for you.

2. Open **MSYS2** from the devkitPro folder in the Start menu (a bash shell that
   comes with devkitPro, with `make` and `git`) and build:

   ```bash
   git clone https://github.com/hjcoggan/nubby-gba.git
   cd nubby-gba
   make
   ```

   If `git` is missing, install it with `pacman -S git`.

3. Install mGBA from <https://mgba.io/downloads.html> and open `nubby-gba.gba`
   with it (or drag the file onto the mGBA window).

### Linux

1. Install devkitPro pacman. On Debian, Ubuntu and derivatives:

   ```bash
   wget https://apt.devkitpro.org/install-devkitpro-pacman
   chmod +x ./install-devkitpro-pacman
   sudo ./install-devkitpro-pacman
   ```

   On Arch and other distros, follow
   <https://devkitpro.org/wiki/devkitPro_pacman>.

2. Install the GBA tools, then log out and back in (or run
   `source /etc/profile.d/devkit-env.sh`) so the environment variables are set:

   ```bash
   sudo dkp-pacman -S gba-dev
   ```

   On Arch-based systems the command is `sudo pacman -S gba-dev` after adding
   the devkitPro repositories.

3. Install mGBA: `sudo apt install mgba-qt`, or from Flathub with
   `flatpak install flathub io.mgba.mGBA`.

4. Build and run:

   ```bash
   git clone https://github.com/hjcoggan/nubby-gba.git
   cd nubby-gba
   make
   mgba-qt nubby-gba.gba
   ```

### Tests

The physics and game rules have host-side tests that build with your normal C compiler:

```bash
make test
```

## Project layout

```
source/main.c     Screens, input, HUD, shop, menus and credits
source/game.c     Pegboard physics, scoring, rounds, items and the shop
source/sound.c    Music and sound effects on the GBA's PSG channels
source/ui.c       Text, panels and menus
source/save.c     Best round and score in SRAM
source/assets.c   Generated: palettes, background images, sprites, font
tools/gen_assets.py  Draws all artwork and writes assets.c/assets.h plus
                     build/preview_*.png
tests/            Host-side tests for the game logic
```

No libraries are needed beyond devkitARM. After editing the artwork in
`tools/gen_assets.py`, run `make assets`.

## AI disclosure

Nearly everything in this repository was generated by Claude (Anthropic) in
Claude Code sessions: the C source, the build setup, the tests, the procedural
artwork generator, the music and sound effects, and this README. A human chose
the features, played the builds and reported what to change. Commits written
this way carry a `Co-Authored-By: Claude` trailer.

The code has host-side tests for the physics and game rules, but it has not
been reviewed line by line by a person, so expect rough edges. Bug reports and
pull requests are welcome.

## License

[MIT](LICENSE) - do whatever you like with it. All code, art and music in this
repo is original; the artwork is generated by `tools/gen_assets.py`.

This is an unofficial fan project inspired by *Nubby's Number Factory*. It is
not affiliated with or endorsed by its creators.
