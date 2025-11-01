# arun

A simple X application runner

## Description

Just an application runner. Goes through your $PATH and adds all binaries. Will try to run the command you provided if nothing is selected.

# Installation

Dependencies

```console
gcc, make, xcb, xcb-randr, xcb-util-keysyms, xcb-randr, X11 (Xlib), X11-xcb (Xlib-xcb), Xft, freetype2 (usually goes with Xft)
```

Install arun

- clone this repo and run `make`, then you can add it to your `/usr/local/bin`

```console
make
sudo cp arun /usr/local/bin
```

# Configuration

Check `config.h` file. Has to be recompiled to apply configuration.
