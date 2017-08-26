// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#pragma once

#include <cassert>

// A macro is used such that external tools won't end up indenting entire files,
// resulting in wasted horizontal space.
#define TVIEW_NAMESPACE_BEGIN namespace tev {
#define TVIEW_NAMESPACE_END }

#define TVIEW_ASSERT(cond, description, ...) assert(cond)
