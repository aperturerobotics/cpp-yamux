package cpp_yamux

import "embed"

// Source embeds the yamux C++ source files for vendoring.
//
//go:embed CMakeLists.txt
//go:embed LICENSE
//go:embed yamux/*.hpp
//go:embed yamux/*.cpp
var Source embed.FS
