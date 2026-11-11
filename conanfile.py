from conan import ConanFile


# What rega plays: the QuickTime / MP4 container and the codecs found in it. Kept in
# step with nix/package.nix and licenses/FFMPEG-BUILD.txt.
FFMPEG_DECODERS = (
    "h264,hevc,prores,dnxhd,mjpeg,mpeg2video,mpeg4,png,qtrle,rawvideo,v210,vp9"
)
FFMPEG_PARSERS = "h264,hevc,mjpeg,mpeg4video,mpegvideo,vp9"


class RegaConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("ftxui/6.1.9")
        self.requires("spdlog/1.16.0")
        self.requires("openexr/3.4.18")
        self.requires("stb/cci.20261021")
        self.requires("libtiff/4.7.2")
        self.requires("zlib/1.3.2")
        # Same range as openexr, so both resolve to one libdeflate.
        self.requires("libdeflate/[>=1.19 <2]")
        self.requires("cli11/2.6.0")
        self.requires("opencolorio/2.5.2")
        self.requires("ffmpeg/9.0.2")
        self.requires("gtest/1.17.0")

    def configure(self):
        self.options["*"].shared = False
        self.options["*"].fPIC = True
        # OCIO reads .ocioz archives through minizip-ng: zlib is all it needs
        # (no openssl, nor the other codecs).
        for opt in ("bzip2", "lzma", "zstd", "openssl", "iconv", "libbsd", "libcomp"):
            setattr(self.options["minizip-ng"], "with_" + opt, False)
        # TIFF textures and plates come uncompressed, LZW, deflate or PackBits
        # (all built in, or from zlib / libdeflate); no other codecs.
        for opt in ("jpeg", "lzma", "zstd", "jbig", "webp", "lerc", "cxx"):
            setattr(self.options["libtiff"], opt, False)
        # Decode only, from local files, with no codec libraries or OS frameworks:
        # libavformat, libavcodec and libswscale's own code plus zlib.
        ffmpeg = self.options["ffmpeg"]
        for opt in ("avdevice", "avfilter", "postproc", "swresample", "with_programs"):
            setattr(ffmpeg, opt, False)
        for opt in (
            "appkit", "audiotoolbox", "avfoundation", "bzip2", "coreimage", "fontconfig",
            "freetype", "fribidi", "harfbuzz", "libalsa", "libaom", "libdav1d", "libdrm",
            "libfdk_aac", "libiconv", "libjxl", "libmp3lame", "libsvtav1", "libvpx",
            "libwebp", "libx264", "libx265", "libxml2", "lzma", "openapv", "openh264",
            "openjpeg", "opus", "pulse", "sdl", "ssl", "vaapi", "vdpau", "videotoolbox",
            "vorbis", "vulkan", "xcb", "xlib",
        ):
            setattr(ffmpeg, "with_" + opt, False)
        ffmpeg.disable_everything = True
        ffmpeg.enable_decoders = FFMPEG_DECODERS
        ffmpeg.enable_demuxers = "mov"
        ffmpeg.enable_parsers = FFMPEG_PARSERS
        ffmpeg.enable_protocols = "file"
