package yamux

import "embed"

// Source embeds the yamux C++ source files for vendoring.
//
//go:embed *.hpp
//go:embed *.cpp
var Source embed.FS
