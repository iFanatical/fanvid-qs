# fanvid-qs

Video wallpapers for [fanshell-qs](https://github.com/iFanatical/fanshell-qs)
on Wayland: an optional add-on, without which fanshell-qs offers only still
images.

A video is decoded once, on the GPU through VA-API, however many outputs
show it. The GPU's video processor converts each frame to RGB at each
output's size, and the frame goes to the compositor as a dmabuf on a
background layer surface. Nothing is drawn in fanvid, and nothing passes
through the CPU.

## Status

`proto/` holds the prototype that settled the design, tested on Hyprland
0.57 with AMD (radeonsi) and Intel (iHD) graphics:

- Hyprland does not show NV12 dmabufs (only their luma plane, as red), and
  does not offer them, so frames are converted to RGB (XBGR8888), which it
  does offer, by VA-API's video processor (`scale_vaapi`).
- Intel's driver converts with BT.601's matrix unless all of the frame's
  colour properties are given; unknown ones are taken as BT.709.
- AMD's driver leaves RGB in limited range unless full range is asked for.
- A 4K HEVC video at 60 fps on two outputs held a steady 62 MB for 18,000
  frames.
