# v4lliveview

FreeBSD V4L2 camera tee with a direct Xlib live window.  It creates a virtual
V4L2 MMAP capture node using the supplied `cuse` kernel module, proxies its
single client to a physical V4L2 device, and shows the newest YUYV frame.

## Build and run

```sh
make
kldload cuse                 # once, if it is not already loaded
sudo ./v4lliveview /dev/video0
```

The default virtual node is `/dev/video-live0`, mode `0660`, with the owner
and group of the process that launches it.  Point the client at that node:

```python
simplepycam.Camera('/dev/video-live0')
```

The application is foreground-only. Closing its X11 window removes the virtual
node and releases the physical camera. It supports YUYV and forwards format,
frame interval, exposure, and white-balance requests to the physical device.

## Scope and safety

The first opener owns the virtual device. Frames are intentionally dropped when
the client has no queued virtual buffer or when the X11 window cannot keep up;
there is no frame queue. Other V4L2 controls and pixel formats are rejected.

`cuse` must be loaded before launch. On this host `/dev/cuse` is restricted to
the administrator, so the daemon normally needs to be launched through an
administrator-approved mechanism (the example uses `sudo`). Loading CUSE and
granting access to the physical camera are administrator actions; the program
does not invoke privilege escalation or `kldload` itself.
