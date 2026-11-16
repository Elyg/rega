# Local conan recipes

Recipes conancenter has but rega needs changed, exported into the conan cache before
`conan install` (`just deps`, `just windows-check` and the release jobs do it):

    conan export conan-recipes/opencolorio --version 2.5.2

- **opencolorio** — conancenter's recipe as of 2.5.2, minus the check that refuses clang
  with libc++. OCIO 2.5 builds with libc++ (Apple clang's on macOS), and llvm-mingw, which
  rega's Windows build uses, has no other C++ library. One patch more
  (`patches/2.5.2-0002-…`): on Windows OCIO opens files by a `std::wstring` name, an MSVC
  extension libc++ lacks; its `c_str()` works with both. And no OCIO apps on Windows (they
  would need minizip-ng's bcrypt, which its package doesn't list), and a static OCIO
  tells MinGW builds too that it is static (`OpenColorIO_SKIP_IMPORTS`). Drop this once conancenter
  does.
