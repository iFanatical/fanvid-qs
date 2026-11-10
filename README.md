# fanvid-qs

Video wallpapers for [fanshell-qs](https://github.com/iFanatical/fanshell-qs)
on Wayland: an optional add-on, without which fanshell-qs offers only still
images.

A video is decoded once, on the GPU through VA-API, however many outputs
show it. The GPU's video processor converts each frame to RGB at each
output's size, and the frame goes to the compositor as a dmabuf on a
background layer surface, which crops, scales and places it. Nothing is
drawn in fanvid, and no frame passes through the CPU. An output nobody can
see gets no frames, and when none can, decoding stops.

## Building

Needs FFmpeg (libavformat, libavcodec, libavfilter, libavutil, libswscale),
libva, libdrm, wayland-client, wayland-protocols and wayland-scanner, and a
compositor with wlr-layer-shell, linux-dmabuf v4, wp_viewporter and
wp_single_pixel_buffer (wp_fractional_scale and wp_alpha_modifier are used
when there).

    make
    sudo make install        # /usr/local/bin/fanvid

[fanhypr-qs](https://github.com/iFanatical/fanhypr-qs)'s `install.sh` asks
whether to install video wallpapers, and if so clones, builds and installs
this, with its dependencies: `tools/need.sh` lists them, in the format of
fanshell-qs's `tools/lib.sh`.

## Protocol

fanshell-qs runs fanvid and talks to it as below, and takes it only when
`fanvid --protocol` prints the version it knows (now 1).

    fanvid --protocol
    fanvid still <file> <out.png>
    fanvid play <file> <output>:<mode>...

`still` writes the video's first frame, at its own size, as PNG: decoded on
the CPU, so it works where the GPU cannot decode the video. It writes
`<out.png>.part` and renames it.

`play` plays the video, looped and silent, as the wallpaper of each output
named (by its wl_output name, such as `DP-1`), in one of feh's modes:
`fill` (cover, cropping), `max` (fit, with black bars), `scale` (stretch),
`center` (unscaled, cropped or with black bars) or `tile` (as `fill`). It
reads commands on stdin, a line each:

| Command | |
|---|---|
| `pause [<output>]` | Stops on the frame shown, on one output or all. |
| `play [<output>]` | Goes on again. |
| `add <output> <mode>` | Plays on another output too, or in another mode. |
| `remove <output>` | Takes it off that output at once. |
| `fade <output> <ms>` | Fades it out on that output, then takes it off. |

It writes on stdout, a line each:

| Message | |
|---|---|
| `playing <output>` | The first frame is on that output. |
| `gone <output>` | It is off that output: removed, faded, or the output went away. |

It exits when stdin closes, when no outputs are left, or on SIGTERM. An
output that goes away (unplugged) is played on again when it comes back.

Exit statuses: 0 done, 1 an error, 2 a usage error, 3 the GPU cannot decode
the video (the caller shows its still instead).

## Notes

Settled by the prototype in `proto/`, on Hyprland 0.57 with AMD (radeonsi)
and Intel (iHD) graphics:

- Hyprland does not show NV12 dmabufs (only their luma plane, as red), and
  does not offer them, so frames are converted to RGB (XBGR8888), which it
  does offer, by VA-API's video processor (`scale_vaapi`).
- Intel's driver converts with BT.601's matrix unless all of the frame's
  colour properties are given; unknown ones are taken as BT.709.
- AMD's driver leaves RGB in limited range unless full range is asked for.

`FANVID_STATS=1` reports frames, frames held, surfaces cached and RssAnon
every 10 s on stderr. `FANVID_LAYER=overlay` puts the video over the
windows, for screenshots in tests.
