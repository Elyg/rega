{
  lib,
  stdenv,
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
  # for a static link, so only ZLIB::ZLIB (found by rvtui anyway) can be among them.
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
  pname = "rvtui";
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
  ];
  checkInputs = [ gtest ];

  cmakeFlags = [
    (lib.cmakeFeature "RVTUI_VERSION" version)
    (lib.cmakeBool "RVTUI_BUILD_TESTS" true)
  ];

  # Tests write temporary EXRs; fine in the sandbox.
  doCheck = true;

  meta = {
    description = "Terminal image browser/viewer for OpenEXR (layers, AOVs, sequences), TIFF, PNG and JPEG";
    mainProgram = "rvtui";
    platforms = lib.platforms.unix;
  };
}
