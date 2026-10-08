#pragma once

// Precompiled header of Core and of the projects built on it (Premake: usePrecompiledHeader),
// force-included into every file. Only system and third-party headers: they are most of what
// every file parses (the standard library alone is about two thirds), and they never change.
// Engine headers stay out, so editing one does not rebuild the whole project.
//
// Files must still include what they use: the precompiled header only makes it cheap. Generate
// the projects with --nopch to check (Launcher and game modules never use one).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
// The Agility SDK's, which comes first on the include path.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <compare>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iosfwd>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
