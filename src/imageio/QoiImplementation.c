// This file was developed by Tiago Chaves & Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#define QOI_NO_STDIO
#define QOI_IMPLEMENTATION
#include <qoi.h>

// This is kept in a separate file to make sure that qoi.h is compiled as C99
// code instead of C++ as converting 'void*' to 'char*' would require casting.
// We define QOI_NO_STDIO because we are using qoi_decode/qoi_encode directly,
// hence, we do not need qoi_read/qoi_write to be include.
