from conan import ConanFile


class RvtuiConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("ftxui/6.1.9")
        self.requires("spdlog/1.16.0")
        self.requires("openexr/3.4.14")
        self.requires("stb/cci.20240531")
        self.requires("libtiff/4.7.1")
        self.requires("zlib/1.3.1")
        # Same range as openexr, so both resolve to one libdeflate.
        self.requires("libdeflate/[>=1.19 <2]")
        self.requires("cli11/2.6.0")
        self.requires("opencolorio/2.5.2")
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
