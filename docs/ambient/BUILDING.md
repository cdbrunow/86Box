# Building and testing the ambient-sounds fork on Linux

These are the exact steps I used on Ubuntu 24.04 (x86-64, GCC 13,
CMake 3.28, Qt 6.4.2) against `master` at `141ae59`. Upstream CI builds on
Ubuntu 26.04 (see `.github/workflows/cmake_linux.yml`). Two of the steps
below are only needed because 24.04 is older than that.

## 1. Install dependencies

This is the CI package list, with SDL2 in place of SDL3 (24.04 has no
`libsdl3-dev`):

```sh
sudo apt update
sudo apt install build-essential ninja-build cmake \
  libfreetype-dev libgles2-mesa-dev libsdl2-dev libpng-dev libc6-dev \
  librtmidi-dev libopenal-dev libslirp-dev libfluidsynth-dev \
  libvdeplug-dev libserialport-dev libzstd-dev libarchive-dev \
  qt6-base-dev qt6-base-private-dev qt6-tools-dev qt6-tools-dev-tools \
  qt6-l10n-tools libevdev-dev libxkbcommon-x11-dev libvulkan-dev \
  fluidsynth clang-format sox
```

`fluidsynth` (the command-line program) is listed on purpose. Ubuntu
24.04's `libfluidsynth-dev` ships a CMake file that points at
`/usr/bin/fluidsynth`, and configuring fails with *"The imported target
FluidSynth::fluidsynth references the file /usr/bin/fluidsynth but this
file does not exist"* if it's missing.

## 2. Configure and build

```sh
cd 86Box
cmake -S . -B build --preset regular -D QT=ON -D USE_QT6=ON -D SDL2=ON
cmake --build build -j"$(nproc)"
```

- `--preset regular` is a Release build. Use `--preset debug` when you want
  to step through code in `gdb`.
- `-D SDL2=ON` is only needed on distros without SDL3. Drop it on 26.04.
- The binary ends up at `build/src/86Box`.

### Known upstream build break on Qt older than 6.7

`src/qt/qt_settingsfloppycdrom.cpp` (~L333, ~L931) and its `.hpp` (~L41)
use `QCheckBox::checkStateChanged` whenever Qt ≥ **6.0**, but that signal
only exists from Qt **6.7**. On Qt 6.4 the build fails with
*"'checkStateChanged' is not a member of 'QCheckBox'"*. The repo already has
a macro for this (`CHECK_STATE_CHANGED` in `qt_defs.hpp`); this file just
doesn't use it.

To build locally, change those three `QT_VERSION_CHECK(6, 0, 0)` guards to
`QT_VERSION_CHECK(6, 7, 0)`. **This fix is not committed.** It has nothing to
do with ambient sounds, and the plan says to stay in scope. On a distro with
Qt ≥ 6.7 you don't need it.

## 3. Point 86Box at the assets fork

`asset_fopen()` (`src/mem/rom.c` ~L205) turns a path starting with
`assets/` into each search directory in turn and uses the first one where
the file opens. The order, from the start-up log, is:

1. `--assetpath <dir>` (or `-A <dir>`) on the command line
2. `<vm folder>/assets/`
3. `<folder containing the 86Box binary>/assets/`
4. inside the AppImage (`$APPDIR/usr/{local/,}share/86Box/assets/`)
5. Qt's data directories: `~/.local/share/86Box/assets/`,
   `/usr/local/share/86Box/assets/`, `/usr/share/86Box/assets/`

The assets repo's root *is* the `assets/` directory (it contains `sounds/`),
so either of these works:

```sh
./build/src/86Box --assetpath /path/to/assets-fork      # per run
ln -s /path/to/assets-fork ~/.local/share/86Box/assets  # permanent
```

The log file (`--logfile 86box.log`) prints every `# Asset path:` it will
search, which is a quick way to check.

You also need the ROM set (`86Box/roms`) on a ROM path, or no machine will
boot.

## 4. Check the existing sounds

1. Create a VM with an AT-class machine and one MFM/IDE hard disk.
2. **HDD:** Settings → Hard disks → pick the disk → set *Speed* to a
   3600 RPM preset → *Audio* to e.g. "IBM H3171, 3600 RPM, 1993". The audio
   list only shows profiles whose RPM matches the speed preset.
3. **Floppy:** Settings → Floppy & CD-ROM drives → pick a drive → choose an
   *Audio* profile. Floppy sounds are also controlled by the global
   `fdd_sounds_enabled` setting (default on).
4. Start the VM. You should hear the HDD spin up, then a steady whine, a tick
   on each seek (`dir /s` in DOS is good for this), and floppy steps and
   motor noise on `dir a:`.

## 5. Formatting

Run `clang-format` on the lines you change only, not whole files:

```sh
git clang-format    # formats only the lines in your staged changes
```

## Building on macOS

Taken from upstream's macOS CI (`.github/workflows/cmake_macos.yml`), which
builds with Homebrew and Qt 5 on Apple Silicon. Qt 6 also works. The
`ambient/integration` branch has been built here against both Qt 5.15 and
Qt 6.4 (on Linux), but not yet on a Mac.

```sh
xcode-select --install          # compilers, once
# Homebrew from https://brew.sh, if you don't have it

brew install cmake ninja sdl3 rtmidi openal-soft fluidsynth libslirp vde \
  libserialport vulkan-headers zstd libarchive qt@5

git clone -b ambient/integration https://github.com/cdbrunow/86Box.git
git clone -b ambient/integration https://github.com/cdbrunow/assets.git
git clone --depth 1 https://github.com/cdbrunow/roms.git

cd 86Box
cmake -S . -B build --preset regular \
  -D QT=ON -D USE_QT6=OFF \
  -D Qt5_ROOT="$(brew --prefix qt@5)" \
  -D Qt5LinguistTools_ROOT="$(brew --prefix qt@5)" \
  -D OpenAL_ROOT="$(brew --prefix openal-soft)" \
  -D LIBSERIALPORT_ROOT="$(brew --prefix libserialport)" \
  -D LibArchive_ROOT="$(brew --prefix libarchive)"
cmake --build build
cmake --install build           # makes build/artifacts/86Box.app
```

For Qt 6, `brew install qt` instead of `qt@5`, and use `-D USE_QT6=ON
-D Qt6_ROOT="$(brew --prefix qt)" -D Qt6LinguistTools_ROOT="$(brew --prefix qt)"`.
Upstream CI uses `--preset development` (adds dev-branch devices);
`regular` is the release configuration.

ROMs and assets: either pass them on the command line,

```sh
build/artifacts/86Box.app/Contents/MacOS/86Box \
  --rompath ../roms --assetpath ../assets
```

or put them where 86Box looks by default (symlinks are fine):

```sh
mkdir -p ~/Library/Application\ Support/net.86box.86Box
ln -s "$PWD/../roms"   ~/Library/Application\ Support/net.86box.86Box/roms
ln -s "$PWD/../assets" ~/Library/Application\ Support/net.86box.86Box/assets
```

On macOS the Action menu shows two power items: **Power** is the new switch
(off keeps the window open), and **Power off** is upstream's, which exits
86Box. Other platforms hide upstream's.
