@CLAUDE.md

This project uses C++20 named modules plus the C++23 `std` and `std.compat` modules. The checked-in preset is
verified with CMake 4.3.4, Homebrew LLVM, and the Ninja generator; configure it with
`cmake --preset modules`. The preset pins the CMake 4.3.4 import-std UUID
`451f2fe2-a8a2-47c3-bc32-94786d8fc91b` and Homebrew's module metadata at
`/opt/homebrew/opt/llvm/lib/c++/libc++.modules.json`; update the version-specific UUID when upgrading CMake.
