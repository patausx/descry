# building descry

## requirements

- devkitpro with devkitarm and the `3ds-dev` packages, including libctru, citro2d and citro3d;
- gnu make and a posix shell;
- makerom and bannertool on `PATH` for cia/cci packages only.

in an existing devkitpro installation:

```sh
sudo dkp-pacman -S --needed 3ds-dev
export DEVKITPRO=/opt/devkitpro
export DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
make
```

this produces `descry.3dsx`, `descry.elf` and `descry.smdh`. see the
[user guide](docs/GUIDE.md#installation) for installation and data paths.

## other packages

```sh
make cia    # descry.cia; requires makerom + bannertool
make cci    # descry.3ds; requires makerom + bannertool
```

## host tests

```sh
make tests
```

these use the system `g++` rather than devkitarm. host tests do not establish
hardware performance, audio quality or console compatibility.

## readme artwork

normal application builds use the existing assets; do not regenerate them just
to compile. the readme banner is separate from the application's icon and splash.
with python 3 and pillow available:

```sh
python3 branding/make_banner.py          # update docs/banner.png only
python3 branding/make_banner.py --check  # verify without writing
```
