# v4lliveview

FreeBSD V4L2 camera tee using [CUSE](https://man.freebsd.org/cgi/man.cgi?query=cuse), with an _optional_ X11 preview. It forwards
original YUYV or MJPEG camera payloads and original V4L2 buffer metadata without
recompression. Cameras remain open while the tee process runs; capture clients
should also keep their stream open for the complete acquisition session.

## Canonical builds

```sh
make
```

The repository has two executable variants, both built directly from the same
source with separate compile flags:

- `v4lliveview`: foreground X11 preview; one virtual port per process.
- `v4lliveview-background`: no GUI; four virtual ports per process by default.

The foreground build uses already-installed CUSE, X11 and libjpeg libraries.
The background build uses CUSE and pthreads and forwards MJPEG without decoding.
No build installs dependencies or changes services. `make BACKGROUND=1` builds
only the background executable. Use `make clean` before changing
`BACKGROUND_PORTS`, or `make -B BACKGROUND_PORTS=N`.

## One or two cameras

Loading cuse if it's not available:

```sh
sudo kldload cuse
```

With CUSE available to start the camera processes, for example on `/dev/video2` and `/dev/video4`:

```sh
sudo ./v4lliveview /dev/video2 0
sudo ./v4lliveview /dev/video4 4
```

These map the respective physical cameras to `/dev/video_live0` and
`/dev/video_live4`. The optional final number is the first virtual-port index;
it defaults to zero. Background processes allocate consecutive ranges, so
choose non-overlapping ranges (for example 0..3 and 4..7).

The first client is the control owner. Additional background ports are monitor
clients and cannot change format, frame interval or forwarded camera controls.
Unknown controls remain rejected.

## Preview and capture behavior

The GUI owns a decoded copy of the last valid image. `STREAMOFF` or closing a
capture client leaves that image visible; exposing the window repaints it.
A new stream's format does not reinterpret the cached old frame. Damaged JPEG
frames are rejected without replacing the last valid preview. Closing the GUI
itself terminates that tee and removes its virtual node.

Preview decoding happens outside the acquisition lock and only for new frames.
It scales the preview to fit roughly 960x720 using JPEG decoder scaling or `YUYV`
sampling. Full original capture payloads are unaffected by preview scaling.
Unselected frames may be dropped when no virtual buffer is queued; this is not
a recording service. Clients must persist selected observations themselves.
Oversized/truncated copies are flagged as invalid rather than silently accepted.

`MJPEG` is camera-side lossy compression; higher advertised frame rate does not
establish scientific equivalence to `YUYV`. On the IW 3000 that has been used
during development, full-size YUYV advertises 1 fps while 3840x3104 MJPEG
advertises 15 fps. Actual exposure, scene, host load and recording cost
may lower delivery rates.

## Timestamp limitation

The tee preserves source timestamps and flags. Tests on the attached cameras
found epoch-scale timestamps carrying a MONOTONIC flag. The tee does not rewrite
those timestamps or pretend they are verified exposure times. A control client
must validate the clock/source contract and distinguish fresh receipt from a
proven post-action exposure.

