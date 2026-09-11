# v4lliveview — design proposal

## Purpose

`v4lliveview` is one local FreeBSD C program using FreeBSD's supplied CUSE
kernel module.  Together they own one physical Video4Linux camera,
transparently proxy a host application's V4L2 session, and tee each captured
frame to:

1. an unmodified third-party V4L2 capture application opening a new virtual
   `/dev/videoN` node; and
2. a small X11 live-view program displaying the same acquisition.

The virtual node must look like a capture device, rather than a pipe, image
sequence, or network stream.  The first supported source is `/dev/video0`;
production will use a different camera.  It is expected to deliver YUYV, but
its dimensions and frame rate are not fixed by this project.

## Evidence and constraints observed on the target

The target is FreeBSD 14.3.  `/dev/video0` is a `webcamd`-provided V4L2 device
with capture and streaming capabilities.  Its current active format is YUYV
4:2:2, 640 x 480, 1280 bytes/line, 614400 bytes/image, at 30 fps.  Its driver
is reported as `uvcvideo` (Linux 5.17 API compatibility).  The system has the
Linux V4L2 UAPI headers (`linux/videodev2.h`) and FreeBSD's CUSE interface.

This is not a Linux host, so Linux `v4l2loopback` cannot be an implementation
dependency.  Merely copying bytes to a new character device is also not
sufficient: normal V4L2 clients negotiate formats and use ioctls, buffer
queues, `poll(2)`, and commonly `mmap(2)`.

## Proposed architecture

```text
 virtual host app                     /dev/video0
 simplepycam                       physical camera
       |                                  ^
       v                                  |
 /dev/video_live0 -> proxy/tee bridge ------+
                         |
                         +--> Xlib: draw current frame
```

### 1. Proxy/tee capture path

The program is the only opener of the physical camera.  It does **not** choose
an independent capture sequence or configuration.  Instead, the active host
application opens `/dev/video_live0`; the module forwards its supported V4L2
open, format, parameter, control, buffer, stream, and queue operations to the
program, which performs them on the physical node in the same order.  The host
application therefore drives the camera through the virtual interface and is
not interrupted by a competing capture session.

On physical `DQBUF`, the program immediately copies the frame into the host
application's corresponding virtual buffer, updates the Xlib current-frame
surface, signals the virtual node readable, then requeues the physical buffer
without waiting for X11 drawing.  It preserves source sequence/timestamp and
`bytesused` metadata.  The only retained image is the one being drawn; there
is no multi-frame cache and no promise to deliver every frame to a slow
consumer.

### 2. Frame hand-off and latency policy

The viewer has exactly one complete current-frame surface.  It holds no queue
or frame history.  The proxy never waits for X11: a slow viewer loses old
frames, not camera acquisition.

The Xlib drawing loop always displays the newest complete frame, favouring the
lowest latency.  If no virtual client buffer is queued when a physical frame
arrives, that frame is dropped for that client; the physical buffer is still
requeued immediately.  Drop counts go to stderr.  This prevents unbounded RAM
use and prevents client or display delay from stalling the physical camera.

No transformation is made in the capture path for version 1.  Pixels delivered
to the virtual device are exactly the physical camera's negotiated YUYV source
format.  The program returns runtime width, height, `bytesperline`, `sizeimage`,
timing, and colour metadata; none are compiled in.  The viewer may convert
YUYV to XRGB locally for display; conversion must not alter virtual-device
bytes.

### 3. Virtual V4L2 output backend

This is the key feasibility boundary.  It exposes `/dev/video_live0` and
implements a deliberately small, coherent capture-device profile:

* `VIDIOC_QUERYCAP`, `ENUM_FMT`, `ENUM_FRAMESIZES`,
  `ENUM_FRAMEINTERVALS`, `G_FMT`, `TRY_FMT`, `S_FMT`, `G_PARM` and `S_PARM`
  describe and configure the runtime-negotiated YUYV stream;
* `VIDIOC_QUERYCTRL`/`QUERY_EXT_CTRL`, `G_CTRL`/`S_CTRL`, and
  `G_EXT_CTRLS`/`S_EXT_CTRLS` expose the physical camera's supported exposure
  and white-balance control families;
* `REQBUFS`, `QUERYBUF`, `QBUF`, `DQBUF`, `STREAMON`, and `STREAMOFF` implement
  a per-client capture queue;
* `poll(2)` signals readable only when a queued client buffer can be dequeued;
* `mmap(2)` is supported for the advertised `MMAP` buffer type; and
* a narrowly documented read-only control set is returned or unsupported
  ioctls fail predictably with `EINVAL`/`ENOTTY` as appropriate.

The backend must not advertise `V4L2_CAP_STREAMING`, a memory type, pixel
format, selection API, or controls it cannot actually honour.  It supports
YUYV only in version 1 but enumerates and accepts every requested YUYV
geometry/frame interval that the physical camera accepts.  It rejects an
unsupported format or geometry rather than silently scaling or converting.

The module forwards supported client operations to the program, which invokes
the corresponding operation on the physical camera and returns the physical
camera's result and actual values.  Thus `VIDIOC_S_FMT`, `VIDIOC_S_PARM`,
`REQBUFS`, `QBUF`, `STREAMON`, `DQBUF`, `STREAMOFF`, and buffer release follow
the host application's sequence.  The module provides the virtual mappings
needed by the client while the program owns the separate physical mappings.
A successful format change invalidates prior virtual buffers; the client must
call `REQBUFS` and map/queue again.  The Xlib window follows the new geometry.

Exposure and white balance are the exception to the general no-control rule.
The virtual node passes their source controls through exactly as reported,
including automatic-mode controls and their paired manual values, ranges,
steps, defaults, and inactive state.  `G_*` returns physical-camera readback;
`S_*` forwards the request to the physical camera and returns its result.
Extended-control batches are applied as a batch.  All other controls are absent
from enumeration and rejected with `EINVAL`.

Initially allow one virtual capture client; a second opener fails with
`EBUSY`.  This preserves ordinary V4L2 buffer ownership and keeps the first
milestone testable.  Multi-client virtual capture can be designed later.

The virtual V4L2 backend uses FreeBSD CUSE.  Its installed kernel module
provides real character-device, MMAP, and poll semantics; the foreground C
program implements per-open V4L2 state and supplies CUSE-backed virtual
buffers.  It remains a genuine device node while avoiding a custom privileged
kernel driver.  CUSE must still pass the exact host-application MMAP,
`select(2)`, dequeue, and requeue sequence on this FreeBSD host.

Do not ship a partial node that merely works with `v4l2-ctl`.

### 4. X11 viewer and launch contract

The program is launched in the foreground as:

```sh
v4lliveview /dev/video0
```

It creates a predictable virtual node (`/dev/video_live0` initially), opens
one X11 window, and displays the current frame when the host application has
started capture through that node.
The program removes its virtual node on clean exit.  A later `--output PATH`
option may select a non-default node; it is not needed in the first interface.

The viewer is linked directly to Xlib/XShm (or Xlib first, with XShm optional).
Qt and all GUI toolkits are explicitly out of scope.  Its only responsibilities
are window lifecycle, aspect-correct presentation, basic resize handling, and
drawing the latest frame.  No overlays, menus, recording, controls, IPC socket,
or second executable are included.  It never opens the physical camera a
second time.

For the expected YUYV source it converts runtime dimensions into a
viewer-owned XRGB framebuffer.
The conversion must respect V4L2 `bytesperline`, colorspace, Y'CbCr encoding,
quantization, and transfer function metadata instead of assuming all YUYV is
full-range BT.601.  The first UI intentionally has no camera-control widgets.

### 5. Lifecycle and permissions

The program opens the physical camera exclusively at launch, but capture begins
only when the virtual host client forwards `STREAMON` and ends when it forwards
`STREAMOFF` or the program exits.  Closing the X11 window requests the same
orderly shutdown.  The virtual node exists only while the program is running;
a simplepycam client may attach at any time after it appears.

Virtual nodes are mode `0660` and inherit the daemon's effective owner and
group at creation.  The daemon account must be permitted to open `/dev/cuse`
and the physical device. No network listener or control socket is in scope.

On source unplug, dequeue/ioctl failure, or format change, the program wakes
the virtual client and ends streaming with the physical-camera error.  It never
feeds stale frames as though live.  A source restart increments stream
generation; the virtual client must renegotiate buffers.

## Process and thread model

One program owns physical-camera access and one module owns virtual-device
state.  Start with two program threads: proxy/capture and Xlib event/draw.
They share one current-frame surface protected by a mutex.  Optimisation to
lock-free structures is explicitly deferred.  Shutdown order is: reject/wake
virtual clients, perform physical `STREAMOFF` if active, release mappings and
buffers, destroy the X window, remove the output node.

## Non-goals for the first milestone

* camera controls other than exposure and white balance;
* compression, recording, network streaming, Wayland, audio, and GPU rendering;
* arbitrary resolution/pixel-format conversion for virtual clients;
* zero-copy end-to-end delivery;
* simultaneous multiple virtual capture nodes in the foreground build;
* command-line configuration beyond physical source path (and later optional
  output-node path).

The background-service build is selected by a compiler flag.  It has no X11
window and creates a configured number of virtual nodes from one physical
camera.  The first successful opener is the control owner for format,
parameter, and control changes until it closes.  Other virtual nodes are
monitoring consumers and receive only the newest frame for their queued
buffers.  When the owner closes, the next new opener becomes control owner.

## Acceptance plan

1. Record physical-source capabilities and enumerate its YUYV geometries/frame
   intervals.  Through the virtual node, use simplepycam to set a supported
   YUYV geometry, verify the returned actual format, and start/stop capture
   repeatedly without leaked mappings or stuck camera state.
2. From the virtual node, enumerate and set supported exposure and
   white-balance controls; compare source readback, virtual-node readback, and
   visible frames.  Verify that automatic/manual state transitions preserve
   the source-reported inactive flags.
3. Verify the virtual node’s advertised capabilities and all supported ioctl
   results with `v4l2-ctl`.
4. Run simplepycam's actual static-image capture path against
   `/dev/video_live0` while the X11 viewer runs; compare sequence/timestamps
   and checksums for frames received by each consumer.  Use `v4l2-ctl` only as
   a supplementary protocol probe, not as the compatibility verdict.
5. Deliberately slow X11 drawing and client buffer requeue, proving that the
   physical camera continues and that only current-frame/client delivery is
   dropped; no frame queue may grow.
6. Close the X11 window and stop simplepycam; verify no deadlock and that the
   virtual node is removed.  Separately stop the source; verify clear error
   behaviour and successful recovery only after buffer renegotiation.
7. Test with the specific third-party application that motivated the project;
   generic `v4l2-ctl` success alone is not compatibility proof.

## Resolved decisions

* The first opener is the camera-control owner until it closes; later virtual
  ports in the background build are monitoring-only.
* If a client has no queued buffer, its frame is dropped and the physical
  buffer is immediately requeued.  Low latency takes priority over delivery.
* Virtual nodes are mode `0660` and use the daemon's owner and group.
