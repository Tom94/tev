// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#pragma once

#include <tinyformat.h>

// A macro is used such that external tools won't end up indenting entire files,
// resulting in wasted horizontal space.
#define TEV_NAMESPACE_BEGIN namespace tev {
#define TEV_NAMESPACE_END }

#define LIKELY(condition) __builtin_expect(static_cast<bool>(condition), 1)
#define UNLIKELY(condition) __builtin_expect(static_cast<bool>(condition), 0)

#define TEV_ASSERT(cond, description, ...) if (UNLIKELY(!(cond))) std::cerr << tfm::format(description, ##__VA_ARGS__) << std::endl;
