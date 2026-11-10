# fanvid's dependencies, for a session repo's install.sh to check with the
# shell's, so one question installs them all. In the need() format of
# fanshell-qs's tools/lib.sh, which must be sourced first:
#
#     . "$shell_dir/tools/lib.sh"
#     . "$fanvid_dir/tools/need.sh"
#     need_fanvid
#
# POSIX sh.

# An ffmpeg that decodes H.264 and HEVC: Fedora's ffmpeg-free decodes
# neither, RPM Fusion's ffmpeg both.
_fanvid_ffmpeg_h264_hevc() {
    command -v ffmpeg >/dev/null 2>&1 || return 1
    _decoders=$(ffmpeg -hide_banner -decoders 2>/dev/null)
    case $_decoders in *" h264 "*) ;; *) return 1 ;; esac
    case $_decoders in *" hevc "*) return 0 ;; *) return 1 ;; esac
}

# A VA-API driver that decodes H.264 and HEVC on this machine's GPU.
_fanvid_vaapi_h264_hevc() {
    command -v vainfo >/dev/null 2>&1 || return 1
    _profiles=$(vainfo 2>/dev/null)
    case $_profiles in *VAProfileH264*VAEntrypointVLD*) ;; *) return 1 ;; esac
    case $_profiles in *VAProfileHEVC*VAEntrypointVLD*) return 0 ;; *) return 1 ;; esac
}

# The GPU's VA-API driver, by vendor. Nothing for a GPU without one
# (NVIDIA's proprietary driver): fanvid then shows each video's first frame.
_fanvid_vaapi_driver() {
    _gpus=$(lspci 2>/dev/null)
    case $_gpus in
    *VGA*Intel* | *Display*Intel*)
        need fn:_fanvid_vaapi_h264_hevc intel-media-driver \
            media-libs/libva-intel-media-driver intel-media-driver \
            "decoding video wallpapers on the Intel GPU"
        ;;
    *VGA*AMD* | *VGA*ATI* | *Display*AMD*)
        # On Gentoo this is media-libs/mesa with USE vaapi.
        need fn:_fanvid_vaapi_h264_hevc mesa-va-drivers-freeworld media-libs/mesa \
            libva-mesa-driver "decoding video wallpapers on the AMD GPU"
        ;;
    esac
}

need_fanvid() {
    need pkg-config pkgconf-pkg-config dev-util/pkgconf pkgconf "building fanvid"
    need make make dev-build/make make "building fanvid"
    need "${CC:-cc}" gcc sys-devel/gcc gcc "building fanvid"
    # Fedora: RPM Fusion's, with the H.264 and HEVC decoders; ffmpeg-devel
    # comes with ffmpeg, which replaces ffmpeg-free and its -devel packages.
    for _module in libavformat libavcodec libavfilter libavutil libswscale; do
        need "pc:$_module" ffmpeg-devel media-video/ffmpeg ffmpeg "fanvid (FFmpeg)"
    done
    need fn:_fanvid_ffmpeg_h264_hevc "ffmpeg ffmpeg-devel" media-video/ffmpeg ffmpeg \
        "H.264 and HEVC video wallpapers"
    need pc:libva libva-devel media-libs/libva libva "fanvid (VA-API)"
    need pc:libdrm libdrm-devel x11-libs/libdrm libdrm "fanvid"
    need pc:wayland-client wayland-devel dev-libs/wayland wayland "fanvid"
    need wayland-scanner wayland-devel dev-util/wayland-scanner wayland "building fanvid"
    need pc:wayland-protocols wayland-protocols-devel dev-libs/wayland-protocols \
        wayland-protocols "building fanvid"
    # Without the driver, fanvid shows each video's first frame instead.
    need_optional_begin
    need vainfo libva-utils media-video/libva-utils libva-utils \
        "checking what the GPU decodes"
    _fanvid_vaapi_driver
    need_optional_end
    # tools/lib.sh prints it when something is still missing on Gentoo.
    gentoo_note="note: on Gentoo, video wallpapers need media-video/ffmpeg with USE vaapi,
      and on AMD media-libs/mesa with USE vaapi"
}
