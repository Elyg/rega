{
  lib,
  stdenv,
  buildPackages,
  cmake,
  ninja,
  pkg-config,
  ftxui,
  spdlog,
  openexr,
  stb,
  libtiff,
  zlib,
  libdeflate,
  cli11,
  opencolorio,
  expat,
  minizip-ng,
  fetchurl,
  nasm,
  gtest,
  version ? "dev",
}:

let
  # expat's autotools build installs CMake files that always import libexpat.so, which
  # a static build doesn't have; point them at libexpat.a so OCIO's find_package works.
  expatStatic = expat.overrideAttrs (old: {
    postFixup = (old.postFixup or "") + ''
      dir=$dev/lib/cmake/expat-${old.version}
      substituteInPlace $dir/expat.cmake \
        --replace-fail 'expat::expat SHARED IMPORTED' 'expat::expat STATIC IMPORTED'
      sed -i -e 's|/libexpat\.so[.0-9]*"|/libexpat.a"|g' -e '/IMPORTED_SONAME/d' $dir/expat-noconfig.cmake
      ! grep -q 'libexpat\.so' $dir/expat-noconfig.cmake
    '';
  });

  # OCIO only needs zlib to read .ocioz archives. nixpkgs' minizip-ng also pulls in
  # openssl/bzip2/xz/zstd, which never reach a static link line; build it zlib-only,
  # as OCIO's own bundled copy is.
  minizipStatic = minizip-ng.overrideAttrs (old: {
    cmakeFlags =
      old.cmakeFlags
      ++ map (opt: lib.cmakeBool opt false) [
        "MZ_OPENSSL"
        "MZ_BZIP2"
        "MZ_LZMA"
        "MZ_ZSTD"
        "MZ_PKCRYPT"
        "MZ_WZAES"
        "MZ_ICONV"
        "MZ_FETCH_LIBS"
      ];
  });

  # TIFF textures and plates need only libtiff's built-in codecs and zlib. nixpkgs'
  # libtiff also links jpeg, xz, zstd, webp and libdeflate, which the static build
  # would compile from source; and its CMake config doesn't name its dependencies
  # for a static link, so only ZLIB::ZLIB (found by rega anyway) can be among them.
  tiffStatic = libtiff.overrideAttrs (old: {
    doCheck = false;
    propagatedBuildInputs = [ zlib ];
    cmakeFlags =
      old.cmakeFlags
      ++ map (opt: lib.cmakeBool opt false) [
        "jpeg"
        "old-jpeg"
        "lzma"
        "zstd"
        "webp"
        "jbig"
        "lerc"
        "libdeflate"
        "tiff-tests"
      ];
  });

  # What rega plays: the QuickTime / MP4 container and the codecs found in it, built
  # the same here and for the static binary. Decode only, from local files, with no
  # codec libraries or OS frameworks. Kept in step with conanfile.py and
  # licenses/FFMPEG-BUILD.txt.
  ffmpegMovies = stdenv.mkDerivation rec {
    pname = "ffmpeg-rega";
    src = import ./ffmpeg-source.nix { inherit fetchurl; };
    inherit (src) version;
    depsBuildBuild = [ buildPackages.stdenv.cc ]; # runs its table generators
    nativeBuildInputs = [ pkg-config ] ++ lib.optional stdenv.hostPlatform.isx86 nasm;
    buildInputs = [ zlib ];
    # FFmpeg's configure is its own: no --build / --host, and static libraries are
    # all it should build.
    configurePlatforms = [ ];
    dontDisableStatic = true;
    configureFlags =
      [
        "--arch=${stdenv.hostPlatform.parsed.cpu.name}"
        "--target_os=${stdenv.hostPlatform.parsed.kernel.name}"
        "--cc=${stdenv.cc.targetPrefix}cc"
        "--pkg-config=pkg-config"
        "--enable-pic"
        "--disable-autodetect"
        "--disable-everything"
        "--disable-programs"
        "--disable-doc"
        "--disable-network"
        "--disable-avdevice"
        "--disable-avfilter"
        "--disable-swresample"
        "--enable-zlib"
        "--enable-demuxer=mov"
        "--enable-protocol=file"
        "--enable-decoder=h264,hevc,prores,dnxhd,mjpeg,mpeg2video,mpeg4,png,qtrle,rawvideo,v210,vp9"
        "--enable-parser=h264,hevc,mjpeg,mpeg4video,mpegvideo,vp9"
      ]
      ++ lib.optionals (stdenv.hostPlatform != stdenv.buildPlatform) [
        "--enable-cross-compile"
        "--cross-prefix=${stdenv.cc.targetPrefix}"
        "--host-cc=${buildPackages.stdenv.cc}/bin/cc"
      ];
    enableParallelBuilding = true;
  };

  # The static (musl) build: no GL apps or python, which have no static libs. Its test
  # suite isn't built either: it's slow to compile and adds nothing to a release.
  ocio =
    if stdenv.hostPlatform.isStatic then
      (opencolorio.override {
        expat = expatStatic;
        minizip-ng = minizipStatic;
        pythonBindings = false;
        buildApps = false;
        glew = null;
        libglut = null;
      }).overrideAttrs
        (old: {
          doCheck = false;
          cmakeFlags = old.cmakeFlags ++ [ (lib.cmakeBool "OCIO_BUILD_TESTS" false) ];
        })
    else
      opencolorio;
in
stdenv.mkDerivation {
  pname = "rega";
  inherit version;

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../CMakeLists.txt
      ../cmake
      ../src
      ../tests
    ];
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
  ];
  buildInputs = [
    ftxui
    spdlog
    openexr
    stb
    (if stdenv.hostPlatform.isStatic then tiffStatic else libtiff)
    zlib
    libdeflate
    cli11
    ocio
    ffmpegMovies
  ];
  checkInputs = [ gtest ];

  cmakeFlags = [
    (lib.cmakeFeature "REGA_VERSION" version)
    (lib.cmakeBool "REGA_BUILD_TESTS" true)
  ];

  # Tests write temporary EXRs; fine in the sandbox.
  doCheck = true;

  meta = {
    description = "Terminal image browser/viewer for OpenEXR (layers, AOVs, sequences), TIFF, PNG, JPEG, MOV and MP4";
    mainProgram = "rega";
    platforms = lib.platforms.unix;
  };
}
