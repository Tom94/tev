/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <tev/Common.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/BmpImageLoader.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/JpegTurboImageLoader.h>
#include <tev/imageio/PngImageLoader.h>

#include <bit>

using namespace nanogui;
using namespace std;

namespace tev {

template <typename T> static T read(const uint8_t* data, bool reverseEndianness) {
    T result = *reinterpret_cast<const T*>(data);
    if (reverseEndianness) {
        result = swapBytes(result);
    }

    return result;
}

// RLE and huffman decoding written by Claude Opus 4.6 + minor fixups. Not reviewed in depth but seems safe.
HeapArray<uint8_t> decode_rle4(const uint8_t* src, size_t src_size, int width, int height) {
    const int stride = nextMultiple(width * 4, 32) / 8;
    HeapArray<uint8_t> dst(stride * height);

    int x = 0;
    int y = 0;
    size_t i = 0;

    const auto set_pixel = [&](int px, int py, uint8_t value) {
        if (px < 0 || px >= width || py < 0 || py >= height) {
            return;
        }

        size_t offset = py * stride + px / 2;
        if (px & 1) {
            dst[offset] = (dst[offset] & 0xF0) | (value & 0x0F);
        } else {
            dst[offset] = (dst[offset] & 0x0F) | ((value & 0x0F) << 4);
        }
    };

    const auto zero_row = [&](int row, int start_x) {
        if (row < 0 || row >= height) {
            return;
        }

        for (int px = start_x; px < width; px++) {
            size_t offset = row * stride + px / 2;
            if (px & 1) {
                dst[offset] &= 0xF0;
            } else {
                dst[offset] &= 0x0F;
            }
        }
    };

    auto zero_pixel = [&](int px, int py) { set_pixel(px, py, 0); };

    // zero first row up to where we start
    zero_row(0, 0);

    while (i + 1 < src_size) {
        uint8_t first = src[i++];
        uint8_t second = src[i++];

        if (first > 0) {
            uint8_t hi = (second >> 4) & 0x0F;
            uint8_t lo = second & 0x0F;
            for (int j = 0; j < first && x < width; j++) {
                set_pixel(x++, y, (j & 1) ? lo : hi);
            }
        } else {
            switch (second) {
                case 0: // end of line
                    zero_row(y, x);
                    x = 0;
                    y++;
                    if (y < height) {
                        zero_row(y, 0);
                    }
                    break;
                case 1: // end of bitmap
                    zero_row(y, x);
                    for (int row = y + 1; row < height; row++) {
                        zero_row(row, 0);
                    }
                    return dst;
                case 2: // delta
                    if (i + 1 >= src_size) {
                        return dst;
                    }
                    {
                        int dx = src[i++];
                        int dy = src[i++];
                    // zero skipped pixels on current row
                        for (int px = x; px < x + dx && px < width; px++) {
                            zero_pixel(px, y);
                        }
                    // zero full rows in between
                        for (int row = y + 1; row < y + dy; row++) {
                            zero_row(row, 0);
                        }
                    // zero start of destination row
                        if (dy > 0) {
                            for (int px = 0; px < x + dx && px < width; px++) {
                                zero_pixel(px, y + dy);
                            }
                        }
                        x += dx;
                        y += dy;
                    }
                    break;
                default: // absolute mode
                {
                    int count = second;
                    int bytes_needed = (count + 3) / 4 * 2;
                    if (i + bytes_needed > src_size) {
                        return dst;
                    }

                    for (int j = 0; j < count && x < width; j++) {
                        uint8_t byte = src[i + j / 2];
                        uint8_t value = (j & 1) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
                        set_pixel(x++, y, value);
                    }
                    i += bytes_needed;
                } break;
            }
        }

        if (y >= height) {
            break;
        }
    }

    // zero remaining
    zero_row(y, x);
    for (int row = y + 1; row < height; row++) {
        zero_row(row, 0);
    }

    return dst;
}

HeapArray<uint8_t> decode_rle8(const uint8_t* src, size_t src_size, int width, int height) {
    const int stride = nextMultiple(width, 4);
    HeapArray<uint8_t> dst(stride * height);

    int x = 0;
    int y = 0;
    size_t i = 0;

    const auto set_pixel = [&](int px, int py, uint8_t value) {
        if (px < 0 || px >= width || py < 0 || py >= height) {
            return;
        }

        dst[py * stride + px] = value;
    };

    const auto zero_row = [&](int row, int start_x) {
        if (row < 0 || row >= height) {
            return;
        }

        for (int px = start_x; px < width; px++) {
            dst[row * stride + px] = 0;
        }
    };

    zero_row(0, 0);

    while (i + 1 < src_size) {
        uint8_t first = src[i++];
        uint8_t second = src[i++];

        if (first > 0) {
            for (int j = 0; j < first && x < width; j++) {
                set_pixel(x++, y, second);
            }
        } else {
            switch (second) {
                case 0: // end of line
                    zero_row(y, x);
                    x = 0;
                    y++;
                    if (y < height) {
                        zero_row(y, 0);
                    }
                    break;
                case 1: // end of bitmap
                    zero_row(y, x);
                    for (int row = y + 1; row < height; row++) {
                        zero_row(row, 0);
                    }
                    return dst;
                case 2: // delta
                    if (i + 1 >= src_size) {
                        return dst;
                    }
                    {
                        int dx = src[i++];
                        int dy = src[i++];
                        for (int px = x; px < x + dx && px < width; px++) {
                            set_pixel(px, y, 0);
                        }
                        for (int row = y + 1; row < y + dy; row++) {
                            zero_row(row, 0);
                        }
                        if (dy > 0) {
                            for (int px = 0; px < x + dx && px < width; px++) {
                                set_pixel(px, y + dy, 0);
                            }
                        }
                        x += dx;
                        y += dy;
                    }
                    break;
                default: // absolute mode
                {
                    int count = second;
                    int padded = (count + 1) & ~1;
                    if (i + padded > src_size) {
                        return dst;
                    }

                    for (int j = 0; j < count && x < width; j++) {
                        set_pixel(x++, y, src[i + j]);
                    }
                    i += padded;
                } break;
            }
        }

        if (y >= height) {
            break;
        }
    }

    zero_row(y, x);
    for (int row = y + 1; row < height; row++) {
        zero_row(row, 0);
    }

    return dst;
}

HeapArray<uint8_t> decode_rle24(const uint8_t* src, size_t src_size, int width, int height) {
    const int stride = nextMultiple(width * 3, 4);
    HeapArray<uint8_t> dst(stride * height);

    int x = 0;
    int y = 0;
    size_t i = 0;

    const auto set_pixel = [&](int px, int py, uint8_t r, uint8_t g, uint8_t b) {
        if (px < 0 || px >= width || py < 0 || py >= height) {
            return;
        }

        int offset = py * stride + px * 3;
        dst[offset + 0] = r;
        dst[offset + 1] = g;
        dst[offset + 2] = b;
    };

    const auto zero_row = [&](int row, int start_x) {
        if (row < 0 || row >= height) {
            return;
        }

        for (int px = start_x; px < width; px++) {
            int offset = row * stride + px * 3;
            dst[offset + 0] = 0;
            dst[offset + 1] = 0;
            dst[offset + 2] = 0;
        }
    };

    zero_row(0, 0);

    while (i + 1 < src_size) {
        uint8_t first = src[i++];
        uint8_t second = src[i++];

        if (first > 0) {
            // encoded run: first = count, next 3 bytes = RGB pixel value
            // second is already read; we need 2 more bytes
            if (i + 1 >= src_size) {
                return dst;
            }

            uint8_t c0 = second;
            uint8_t c1 = src[i++];
            uint8_t c2 = src[i++];

            for (int j = 0; j < first && x < width; j++) {
                set_pixel(x++, y, c0, c1, c2);
            }
        } else {
            switch (second) {
                case 0: // end of line
                    zero_row(y, x);
                    x = 0;
                    y++;
                    if (y < height) {
                        zero_row(y, 0);
                    }
                    break;
                case 1: // end of bitmap
                    zero_row(y, x);
                    for (int row = y + 1; row < height; row++) {
                        zero_row(row, 0);
                    }
                    return dst;
                case 2: // delta
                    if (i + 1 >= src_size) {
                        return dst;
                    }
                    {
                        int dx = src[i++];
                        int dy = src[i++];
                        for (int px = x; px < x + dx && px < width; px++) {
                            set_pixel(px, y, 0, 0, 0);
                        }
                        for (int row = y + 1; row < y + dy; row++) {
                            zero_row(row, 0);
                        }
                        if (dy > 0) {
                            for (int px = 0; px < x + dx && px < width; px++) {
                                set_pixel(px, y + dy, 0, 0, 0);
                            }
                        }
                        x += dx;
                        y += dy;
                    }
                    break;
                default: // absolute mode
                {
                    int count = second;
                    int data_bytes = count * 3;
                    int padded = (data_bytes + 1) & ~1;
                    if (i + padded > src_size) {
                        return dst;
                    }

                    for (int j = 0; j < count && x < width; j++) {
                        set_pixel(x++, y, src[i + j * 3], src[i + j * 3 + 1], src[i + j * 3 + 2]);
                    }
                    i += padded;
                } break;
            }
        }

        if (y >= height) {
            break;
        }
    }

    zero_row(y, x);
    for (int row = y + 1; row < height; row++) {
        zero_row(row, 0);
    }

    return dst;
}

HeapArray<uint8_t> decode_huffman1d(const uint8_t* src, size_t src_size, int width, int height) {
    const int stride = nextMultiple(width, 8) / 8;
    const int padded_stride = nextMultiple(stride, 4);
    HeapArray<uint8_t> dst(padded_stride * height);
    memset(dst.data(), 0, dst.size());

    // Huffman code table entry: {code_bits, code_length, run_length, is_terminating}
    struct HuffEntry {
        uint16_t code;
        uint8_t len;
        uint16_t run;
    };

    // White terminating codes (run 0..63)
    static const HuffEntry white_term[] = {
        {0b00110101, 8, 0 },
        {0b000111,   6, 1 },
        {0b0111,     4, 2 },
        {0b1000,     4, 3 },
        {0b1011,     4, 4 },
        {0b1100,     4, 5 },
        {0b1110,     4, 6 },
        {0b1111,     4, 7 },
        {0b10011,    5, 8 },
        {0b10100,    5, 9 },
        {0b00111,    5, 10},
        {0b01000,    5, 11},
        {0b001000,   6, 12},
        {0b000011,   6, 13},
        {0b110100,   6, 14},
        {0b110101,   6, 15},
        {0b101010,   6, 16},
        {0b101011,   6, 17},
        {0b0100111,  7, 18},
        {0b0001100,  7, 19},
        {0b0001000,  7, 20},
        {0b0010111,  7, 21},
        {0b0000011,  7, 22},
        {0b0000100,  7, 23},
        {0b0101000,  7, 24},
        {0b0101011,  7, 25},
        {0b0010011,  7, 26},
        {0b0100100,  7, 27},
        {0b0011000,  7, 28},
        {0b00000010, 8, 29},
        {0b00000011, 8, 30},
        {0b00011010, 8, 31},
        {0b00011011, 8, 32},
        {0b00010010, 8, 33},
        {0b00010011, 8, 34},
        {0b00010100, 8, 35},
        {0b00010101, 8, 36},
        {0b00010110, 8, 37},
        {0b00010111, 8, 38},
        {0b00101000, 8, 39},
        {0b00101001, 8, 40},
        {0b00101010, 8, 41},
        {0b00101011, 8, 42},
        {0b00101100, 8, 43},
        {0b00101101, 8, 44},
        {0b00000100, 8, 45},
        {0b00000101, 8, 46},
        {0b00001010, 8, 47},
        {0b00001011, 8, 48},
        {0b01010010, 8, 49},
        {0b01010011, 8, 50},
        {0b01010100, 8, 51},
        {0b01010101, 8, 52},
        {0b00100100, 8, 53},
        {0b00100101, 8, 54},
        {0b01011000, 8, 55},
        {0b01011001, 8, 56},
        {0b01011010, 8, 57},
        {0b01011011, 8, 58},
        {0b01001010, 8, 59},
        {0b01001011, 8, 60},
        {0b00110010, 8, 61},
        {0b00110011, 8, 62},
        {0b00110100, 8, 63},
    };

    // White makeup codes (run 64..1728)
    static const HuffEntry white_makeup[] = {
        {0b11011,     5, 64  },
        {0b10010,     5, 128 },
        {0b010111,    6, 192 },
        {0b0110111,   7, 256 },
        {0b00110110,  8, 320 },
        {0b00110111,  8, 384 },
        {0b01100100,  8, 448 },
        {0b01100101,  8, 512 },
        {0b01101000,  8, 576 },
        {0b01100111,  8, 640 },
        {0b011001100, 9, 704 },
        {0b011001101, 9, 768 },
        {0b011010010, 9, 832 },
        {0b011010011, 9, 896 },
        {0b011010100, 9, 960 },
        {0b011010101, 9, 1024},
        {0b011010110, 9, 1088},
        {0b011010111, 9, 1152},
        {0b011011000, 9, 1216},
        {0b011011001, 9, 1280},
        {0b011011010, 9, 1344},
        {0b011011011, 9, 1408},
        {0b010011000, 9, 1472},
        {0b010011001, 9, 1536},
        {0b010011010, 9, 1600},
        {0b011000,    6, 1664},
        {0b010011011, 9, 1728},
    };

    // Black terminating codes (run 0..63)
    static const HuffEntry black_term[] = {
        {0b0000110111,   10, 0 },
        {0b010,          3,  1 },
        {0b11,           2,  2 },
        {0b10,           2,  3 },
        {0b011,          3,  4 },
        {0b0011,         4,  5 },
        {0b0010,         4,  6 },
        {0b00011,        5,  7 },
        {0b000101,       6,  8 },
        {0b000100,       6,  9 },
        {0b0000100,      7,  10},
        {0b0000101,      7,  11},
        {0b0000111,      7,  12},
        {0b00000100,     8,  13},
        {0b00000111,     8,  14},
        {0b000011000,    9,  15},
        {0b0000010111,   10, 16},
        {0b0000011000,   10, 17},
        {0b0000001000,   10, 18},
        {0b00001100111,  11, 19},
        {0b00001101000,  11, 20},
        {0b00001101100,  11, 21},
        {0b00000110111,  11, 22},
        {0b00000101000,  11, 23},
        {0b00000010111,  11, 24},
        {0b00000011000,  11, 25},
        {0b000011001010, 12, 26},
        {0b000011001011, 12, 27},
        {0b000011001100, 12, 28},
        {0b000011001101, 12, 29},
        {0b000001101000, 12, 30},
        {0b000001101001, 12, 31},
        {0b000001101010, 12, 32},
        {0b000001101011, 12, 33},
        {0b000011010010, 12, 34},
        {0b000011010011, 12, 35},
        {0b000011010100, 12, 36},
        {0b000011010101, 12, 37},
        {0b000011010110, 12, 38},
        {0b000011010111, 12, 39},
        {0b000001101100, 12, 40},
        {0b000001101101, 12, 41},
        {0b000011011010, 12, 42},
        {0b000011011011, 12, 43},
        {0b000001010100, 12, 44},
        {0b000001010101, 12, 45},
        {0b000001010110, 12, 46},
        {0b000001010111, 12, 47},
        {0b000001100100, 12, 48},
        {0b000001100101, 12, 49},
        {0b000001010010, 12, 50},
        {0b000001010011, 12, 51},
        {0b000000100100, 12, 52},
        {0b000000110111, 12, 53},
        {0b000000111000, 12, 54},
        {0b000000100111, 12, 55},
        {0b000000101000, 12, 56},
        {0b000001011000, 12, 57},
        {0b000001011001, 12, 58},
        {0b000000101011, 12, 59},
        {0b000000101100, 12, 60},
        {0b000001011010, 12, 61},
        {0b000001100110, 12, 62},
        {0b000001100111, 12, 63},
    };

    // Black makeup codes (run 64..1728)
    static const HuffEntry black_makeup[] = {
        {0b0000001111,    10, 64  },
        {0b000011001000,  12, 128 },
        {0b000011001001,  12, 192 },
        {0b000001011011,  12, 256 },
        {0b000000110011,  12, 320 },
        {0b000000110100,  12, 384 },
        {0b000000110101,  12, 448 },
        {0b0000001101100, 13, 512 },
        {0b0000001101101, 13, 576 },
        {0b0000001001010, 13, 640 },
        {0b0000001001011, 13, 704 },
        {0b0000001001100, 13, 768 },
        {0b0000001001101, 13, 832 },
        {0b0000001110010, 13, 896 },
        {0b0000001110011, 13, 960 },
        {0b0000001110100, 13, 1024},
        {0b0000001110101, 13, 1088},
        {0b0000001110110, 13, 1152},
        {0b0000001110111, 13, 1216},
        {0b0000001010010, 13, 1280},
        {0b0000001010011, 13, 1344},
        {0b0000001010100, 13, 1408},
        {0b0000001010101, 13, 1472},
        {0b0000001011010, 13, 1536},
        {0b0000001011011, 13, 1600},
        {0b0000001100100, 13, 1664},
        {0b0000001100101, 13, 1728},
    };

    // Extended makeup codes (shared for black and white, run 1792..2560)
    static const HuffEntry ext_makeup[] = {
        {0b00000001000,  11, 1792},
        {0b00000001100,  11, 1856},
        {0b00000001101,  11, 1920},
        {0b000000010010, 12, 1984},
        {0b000000010011, 12, 2048},
        {0b000000010100, 12, 2112},
        {0b000000010101, 12, 2176},
        {0b000000010110, 12, 2240},
        {0b000000010111, 12, 2304},
        {0b000000011100, 12, 2368},
        {0b000000011101, 12, 2432},
        {0b000000011110, 12, 2496},
        {0b000000011111, 12, 2560},
    };

    // EOL = 000000000001 (12 bits)
    static const uint16_t EOL_CODE = 0b000000000001;
    static const int EOL_LEN = 12;

    // Bit reader: reads bits MSB-first from the byte stream
    size_t byte_pos = 0;
    int bit_pos = 7; // next bit to read within src[byte_pos], 7 = MSB

    const auto bits_left = [&]() -> size_t {
        if (byte_pos >= src_size) {
            return 0;
        }
        return (src_size - byte_pos - 1) * 8 + bit_pos + 1;
    };

    const auto peek_bits = [&](int n) -> uint32_t {
        uint32_t result = 0;
        size_t bp = byte_pos;
        int bitp = bit_pos;
        for (int i = 0; i < n; i++) {
            if (bp >= src_size) {
                return result;
            }
            result = (result << 1) | ((src[bp] >> bitp) & 1);
            if (--bitp < 0) {
                bitp = 7;
                bp++;
            }
        }
        return result;
    };

    const auto skip_bits = [&](int n) {
        for (int i = 0; i < n; i++) {
            if (byte_pos >= src_size) {
                return;
            }
            if (--bit_pos < 0) {
                bit_pos = 7;
                byte_pos++;
            }
        }
    };

    // Try to match a code from a table. Returns true and sets run_length if matched.
    const auto match_table = [&](const HuffEntry* table, int count, uint16_t& run) -> bool {
        for (int i = 0; i < count; i++) {
            if (bits_left() < (size_t)table[i].len) {
                continue;
            }
            uint32_t bits = peek_bits(table[i].len);
            if (bits == table[i].code) {
                skip_bits(table[i].len);
                run = table[i].run;
                return true;
            }
        }
        return false;
    };

    // Set a run of pixels in the output
    const auto set_run = [&](int row, int start_x, int run_len, bool black) {
        if (!black) {
            return; // white is already 0
        }
        if (row < 0 || row >= height) {
            return;
        }
        uint8_t* row_ptr = dst.data() + row * padded_stride;
        for (int px = start_x; px < start_x + run_len && px < width; px++) {
            row_ptr[px / 8] |= (0x80 >> (px & 7));
        }
    };

    // Skip initial EOL if present
    if (bits_left() >= EOL_LEN && peek_bits(EOL_LEN) == EOL_CODE) {
        skip_bits(EOL_LEN);
    }

    for (int y = 0; y < height; y++) {
        int x = 0;
        bool is_white = true; // each line starts with white

        while (x < width) {
            int total_run = 0;
            bool got_terminating = false;

            while (!got_terminating) {
                if (bits_left() < 2) {
                    goto done;
                }

                uint16_t run = 0;
                bool matched = false;

                // Try extended makeup (shared)
                matched = match_table(ext_makeup, 13, run);

                if (!matched) {
                    if (is_white) {
                        matched = match_table(white_makeup, 27, run);
                        if (!matched) {
                            matched = match_table(white_term, 64, run);
                            if (matched) {
                                got_terminating = true;
                            }
                        }
                    } else {
                        matched = match_table(black_makeup, 27, run);
                        if (!matched) {
                            matched = match_table(black_term, 64, run);
                            if (matched) {
                                got_terminating = true;
                            }
                        }
                    }
                }

                if (!matched) {
                    // Unrecognized code — skip a bit and try to recover
                    skip_bits(1);
                    continue;
                }

                total_run += run;
            }

            set_run(y, x, total_run, !is_white);
            x += total_run;
            is_white = !is_white;
        }

        // Consume EOL marker if present (skip fill bits of 0, then expect 000000000001)
        // Fill bits are zeros before the EOL
        while (bits_left() > 0 && peek_bits(1) == 0) {
            // Could be fill or start of EOL. Check if we have the full EOL.
            if (bits_left() >= EOL_LEN && peek_bits(EOL_LEN) == EOL_CODE) {
                skip_bits(EOL_LEN);
                break;
            }
            skip_bits(1);
        }

        // Check for RTC (6 consecutive EOLs = end of data)
        if (bits_left() >= EOL_LEN * 5) {
            bool is_rtc = true;
            size_t saved_bp = byte_pos;
            int saved_bitp = bit_pos;
            for (int e = 0; e < 5; e++) {
                if (peek_bits(EOL_LEN) != EOL_CODE) {
                    is_rtc = false;
                    break;
                }
                skip_bits(EOL_LEN);
            }
            byte_pos = saved_bp;
            bit_pos = saved_bitp;
            if (is_rtc) {
                goto done;
            }
        }
    }

done:
    return dst;
}

Task<vector<ImageData>> BmpImageLoader::load(
    istream& iStream, const fs::path& path, string_view channelSelector, int priority, const GainmapHeadroom& gainmapHeadroom
) const {
    const bool reverseEndianness = endian::native == endian::big;

    char signature[2];
    iStream.read((char*)&signature, sizeof(signature));
    if (!iStream) {
        throw FormatNotSupported{"Failed to read BMP signature."};
    }

    const string sig = string{signature, 2};
    if (sig != "BM" && sig != "BA" && sig != "CI" && sig != "CP" && sig != "IC" && sig != "PT") {
        throw FormatNotSupported{"Invalid BMP signature."};
    }

    if (sig == "BA") {
        tlog::debug() << fmt::format("Loading BA BMP sequence");

        struct BaHeader {
            // Size of this header and following ones. Not meant to be used; see https://www.fileformat.info/format/os2bmp/egff.htm. Only
            // the offset to next is actually relevant for decoding.
            uint32_t headerSize;
            uint32_t offsetToNext;
            uint16_t width;
            uint16_t height;
        } baFileHeader;

        vector<ImageData> result;
        for (size_t i = 0;; i++) {
            iStream.read((char*)&baFileHeader, sizeof(BaHeader));
            if (!iStream) {
                throw ImageLoadError{"Failed to read BA BMP header."};
            }

            if (reverseEndianness) {
                baFileHeader.headerSize = swapBytes(baFileHeader.headerSize);
                baFileHeader.offsetToNext = swapBytes(baFileHeader.offsetToNext);
                baFileHeader.width = swapBytes(baFileHeader.width);
                baFileHeader.height = swapBytes(baFileHeader.height);
            }

            if (baFileHeader.headerSize < sizeof(BaHeader)) {
                throw ImageLoadError{fmt::format("Invalid BA BMP header size: {}", baFileHeader.headerSize)};
            }

            tlog::debug() << fmt::format(
                "BA BMP frame #{}: headerSize={} offsetToNext={} width={} height={}",
                i,
                baFileHeader.headerSize,
                baFileHeader.offsetToNext,
                baFileHeader.width,
                baFileHeader.height
            );

            try {
                auto tmp = co_await load(iStream, path, channelSelector, priority, gainmapHeadroom);
                for (auto& image : tmp) {
                    image.partName = Channel::joinIfNonempty(fmt::format("frames.{}", i), image.partName);
                }

                result.insert(result.end(), make_move_iterator(tmp.begin()), make_move_iterator(tmp.end()));
            } catch (const FormatNotSupported&) { throw ImageLoadError{"Failed to load BA BMP frame: inner data is not a valid BMP."}; }

            if (baFileHeader.offsetToNext == 0) {
                break;
            }

            iStream.seekg(baFileHeader.offsetToNext, ios::beg);
            iStream.read((char*)&signature, sizeof(signature));
            if (!iStream || string{signature, 2} != "BA") {
                throw ImageLoadError{"Failed to read next BA BMP signature."};
            }
        }

        co_return result;
    }

    struct BmpFileHeader {
        uint32_t fileSize;
        uint16_t reserved0;
        uint16_t reserved1;
        uint32_t pixelDataOffset;
    } fileHeader;
    static_assert(sizeof(fileHeader) == 12, "BmpFileHeader struct must be exactly 12 bytes to match the BMP file header format.");

    iStream.read((char*)&fileHeader, sizeof(BmpFileHeader));
    if (!iStream) {
        throw ImageLoadError{"Failed to read BMP file header."};
    }

    if (reverseEndianness) {
        fileHeader.fileSize = swapBytes(fileHeader.fileSize);
        fileHeader.reserved0 = swapBytes(fileHeader.reserved0);
        fileHeader.reserved1 = swapBytes(fileHeader.reserved1);
        fileHeader.pixelDataOffset = swapBytes(fileHeader.pixelDataOffset);
    }

    co_return co_await loadWithoutFileHeader(
        iStream,
        path,
        channelSelector,
        priority,
        gainmapHeadroom,
        fileHeader.pixelDataOffset > 0 ? make_optional<size_t>(fileHeader.pixelDataOffset) : nullopt
    );
}

Task<vector<ImageData>> BmpImageLoader::loadWithoutFileHeader(
    istream& iStream,
    const fs::path& path,
    string_view channelSelector,
    int priority,
    const GainmapHeadroom& gainmapHeadroom,
    optional<size_t> pixelDataOffset,
    Vector2i* sizeInOut,
    bool alphaByDefault
) const {
    const bool reverseEndianness = endian::native == endian::big;

    struct DibHeader {
        uint32_t size;
        int32_t width;
        int32_t height;
        uint16_t planes;
        uint16_t bitsPerPixel;
        uint32_t compression;
        uint32_t imageSize;
        int32_t xPixelsPerMeter;
        int32_t yPixelsPerMeter;
        uint32_t colorsUsed;
        uint32_t importantColors;
        // The following members differ between OS/2 and Windows BMP formats. We don't really care about them in the case of OS/2 (no
        // actionable info for tev), so we define just the Windows values in this struct
        uint32_t redMask;
        uint32_t greenMask;
        uint32_t blueMask;
        uint32_t alphaMask;
        uint32_t colorSpaceType;
        uint32_t redX;
        uint32_t redY;
        uint32_t redZ;
        uint32_t greenX;
        uint32_t greenY;
        uint32_t greenZ;
        uint32_t blueX;
        uint32_t blueY;
        uint32_t blueZ;
        uint32_t gammaRed;
        uint32_t gammaGreen;
        uint32_t gammaBlue;
        uint32_t intent;
        uint32_t iccProfileData;
        uint32_t iccProfileSize;
        uint32_t reserved;
    } dib = {};
    static_assert(sizeof(dib) == 124, "DibHeader struct must be exactly 124 bytes to match the Windows BMP header v5 format.");

    const size_t dibHeaderBegin = iStream.tellg();

    iStream.read((char*)&dib.size, sizeof(uint32_t));
    if (!iStream) {
        throw ImageLoadError{"Failed to read BMP DIB header size."};
    }

    if (reverseEndianness) {
        dib.size = swapBytes(dib.size);
    }

    size_t bytesToRead = dib.size - sizeof(uint32_t); // We already read the size field

    enum class EType : uint32_t {
        Os2V1 = 12,
        Os2V2 = 64,
        Os2V2_16 = 16, // Same as Os2V2 but everything after the 16th byte zero
        WindowsV1 = 40,
        WindowsV2 = 52,
        WindowsV3 = 56,
        WindowsV4 = 108,
        WindowsV5 = 124,
    };
    const EType type = (EType)dib.size;

    switch (type) {
        case EType::Os2V1:
        case EType::Os2V2:
        case EType::Os2V2_16:
        case EType::WindowsV1:
        case EType::WindowsV2:
        case EType::WindowsV3:
        case EType::WindowsV4:
        case EType::WindowsV5: break;
        default: throw ImageLoadError{fmt::format("Unsupported BMP DIB header size: {}", dib.size)};
    }

    // This particular header uses 16-bit values for width and height. Others 32-bit values, so we have to read those first and then read
    // the rest of the header accordingly.
    if (type == EType::Os2V1) {
        tlog::debug() << "BMP uses OS/2 V1 DIB header with 16-bit width and height";

        uint16_t width16, height16;
        iStream.read((char*)&width16, sizeof(uint16_t));
        iStream.read((char*)&height16, sizeof(uint16_t));

        if (reverseEndianness) {
            width16 = swapBytes(width16);
            height16 = swapBytes(height16);
        }

        dib.width = width16;
        dib.height = height16;

        bytesToRead -= sizeof(uint16_t) * 2;
    } else {
        iStream.read((char*)&dib.width, sizeof(uint32_t));
        iStream.read((char*)&dib.height, sizeof(uint32_t));

        if (reverseEndianness) {
            dib.width = swapBytes(dib.width);
            dib.height = swapBytes(dib.height);
        }

        bytesToRead -= sizeof(uint32_t) * 2;
    }

    if (sizeInOut != nullptr) {
        const auto tmp = Vector2i{dib.width, dib.height};
        dib.width = sizeInOut->x();
        dib.height = sizeInOut->y();
        *sizeInOut = tmp;
    }

    // `dib` was zero initialized, so it's fine to not read the whole struct
    iStream.read((char*)&dib + sizeof(uint32_t) * 3, bytesToRead);
    bytesToRead = 0;

    if (!iStream) {
        throw ImageLoadError{fmt::format("Failed to read BMP DIB header with size {}", dib.size)};
    }

    if (reverseEndianness) {
        dib.planes = swapBytes(dib.planes);
        dib.bitsPerPixel = swapBytes(dib.bitsPerPixel);
        dib.compression = swapBytes(dib.compression);
        dib.imageSize = swapBytes(dib.imageSize);
        dib.xPixelsPerMeter = swapBytes(dib.xPixelsPerMeter);
        dib.yPixelsPerMeter = swapBytes(dib.yPixelsPerMeter);
        dib.colorsUsed = swapBytes(dib.colorsUsed);
        dib.importantColors = swapBytes(dib.importantColors);
        dib.redMask = swapBytes(dib.redMask);
        dib.greenMask = swapBytes(dib.greenMask);
        dib.blueMask = swapBytes(dib.blueMask);
        dib.alphaMask = swapBytes(dib.alphaMask);
        dib.colorSpaceType = swapBytes(dib.colorSpaceType);
        dib.redX = swapBytes(dib.redX);
        dib.redY = swapBytes(dib.redY);
        dib.redZ = swapBytes(dib.redZ);
        dib.greenX = swapBytes(dib.greenX);
        dib.greenY = swapBytes(dib.greenY);
        dib.greenZ = swapBytes(dib.greenZ);
        dib.blueX = swapBytes(dib.blueX);
        dib.blueY = swapBytes(dib.blueY);
        dib.blueZ = swapBytes(dib.blueZ);
        dib.gammaRed = swapBytes(dib.gammaRed);
        dib.gammaGreen = swapBytes(dib.gammaGreen);
        dib.gammaBlue = swapBytes(dib.gammaBlue);
        dib.intent = swapBytes(dib.intent);
        dib.iccProfileData = swapBytes(dib.iccProfileData);
        dib.iccProfileSize = swapBytes(dib.iccProfileSize);
        dib.reserved = swapBytes(dib.reserved);
    }

    enum class ECompression : uint32_t {
        Rgb = 0,
        Rle8,
        Rle4,
        Bitfields,
        Jpeg,
        Png,
        AlphaBitfields,
        Cmyk,
        CmykRle8,
        CmykRle4,
        Huffman,
        Rle24,
    };

    const auto compressionToString = [](ECompression compression) -> string_view {
        switch (compression) {
            case ECompression::Rgb: return "rgb";
            case ECompression::Rle8: return "rle8";
            case ECompression::Rle4: return "rle4";
            case ECompression::Bitfields: return "bitfields";
            case ECompression::Jpeg: return "jpeg";
            case ECompression::Png: return "png";
            case ECompression::AlphaBitfields: return "alpha_bitfields";
            case ECompression::Cmyk: return "cmyk";
            case ECompression::CmykRle8: return "cmyk_rle8";
            case ECompression::CmykRle4: return "cmyk_rle4";
            case ECompression::Huffman: return "huffman";
            case ECompression::Rle24: return "rle24";
            default: return "unknown";
        }
    };

    const auto convertCompression = [&](uint32_t compression) -> ECompression {
        switch (compression) {
            case 0: return ECompression::Rgb;
            case 1: return ECompression::Rle8;
            case 2: return ECompression::Rle4;
            case 3: return type == EType::Os2V2 ? ECompression::Huffman : ECompression::Bitfields;
            case 4: return type == EType::Os2V2 ? ECompression::Rle24 : ECompression::Jpeg;
            case 5: return ECompression::Png;
            case 6: return ECompression::AlphaBitfields;
            case 11: return ECompression::Cmyk;
            case 12: return ECompression::CmykRle8;
            case 13: return ECompression::CmykRle4;
            default: throw ImageLoadError{fmt::format("Invalid BMP compression method: {}", compression)};
        }
    };

    const ECompression compression = convertCompression(dib.compression);

    if (compression == ECompression::Cmyk || compression == ECompression::CmykRle8 || compression == ECompression::CmykRle4) {
        throw ImageLoadError{fmt::format("Unsupported BMP compression method: {}", compressionToString(compression))};
    }

    if (compression == ECompression::Jpeg) {
        tlog::debug() << "BMP embeds JPEG data. Delegating to JPEG loader.";

        if (pixelDataOffset.has_value()) {
            iStream.seekg(*pixelDataOffset, ios::beg);
            if (!iStream) {
                throw ImageLoadError{"Failed to seek to JPEG data in BMP file."};
            }
        }

        const auto jpegLoader = JpegTurboImageLoader{};
        try {
            co_return co_await jpegLoader.load(iStream, path, channelSelector, priority, gainmapHeadroom);
        } catch (const FormatNotSupported&) { throw ImageLoadError{"BMP file uses JPEG compression, but JPEG data is not valid."}; }
    }

    if (compression == ECompression::Png) {
        tlog::debug() << "BMP embeds PNG data. Delegating to PNG loader.";

        if (pixelDataOffset.has_value()) {
            iStream.seekg(*pixelDataOffset, ios::beg);
            if (!iStream) {
                throw ImageLoadError{"Failed to seek to PNG data in BMP file."};
            }
        }

        const auto pngLoader = PngImageLoader{};
        try {
            co_return co_await pngLoader.load(iStream, path, channelSelector, priority, gainmapHeadroom);
        } catch (const FormatNotSupported&) { throw ImageLoadError{"BMP file uses PNG compression, but PNG data is not valid."}; }
    }

    if (dib.bitsPerPixel == 0) {
        throw ImageLoadError{fmt::format("Invalid BMP bits per pixel: {}", dib.bitsPerPixel)};
    }

    const bool hasPalette = dib.bitsPerPixel <= 8;
    if (!hasPalette && dib.bitsPerPixel != 16 && dib.bitsPerPixel != 24 && dib.bitsPerPixel != 32 && dib.bitsPerPixel != 64) {
        throw ImageLoadError{fmt::format("Unsupported BMP bits per pixel for non-paletted image: {}", dib.bitsPerPixel)};
    }

    const auto setColorMasksToDefault = [&]() {
        // If no color masks are provided, BMP defaults to 5 bits RGB for 16bpp and 8 bits RGB for 24bpp and 32bpp. NOTE: the alpha channel
        // is *disabled* by default; see https://en.wikipedia.org/wiki/BMP_file_format#/media/File:AllBMPformats.png
        // However, when BMP images are embedded in other files, e.g. ICO files, this situation changes. That's why we have an optional
        // alpha mask override in the arguments.
        if (!hasPalette && dib.bitsPerPixel == 16) {
            dib.alphaMask = alphaByDefault ? 0x00'00'80'00 : 0;
            dib.redMask = 0x00'00'7C'00;
            dib.greenMask = 0x00'00'03'E0;
            dib.blueMask = 0x00'00'00'1F;
        } else {
            dib.alphaMask = alphaByDefault ? 0xFF'00'00'00 : 0;
            dib.redMask = 0x00'FF'00'00;
            dib.greenMask = 0x00'00'FF'00;
            dib.blueMask = 0x00'00'00'FF;
        }
    };

    // If the DIB header is too small to contain the color masks for BITFIELDS or ALPHABITFIELDS compression, the masks are stored in the
    // following bytes. This is a weird quirk of the BMP format but we have to support it.
    if (dib.size < 52) {
        if (compression == ECompression::Bitfields) {
            tlog::debug()
                << "BMP uses BITFIELDS compression but DIB header is too small to contain color masks; reading masks from following bytes";

            uint32_t masks[3];
            iStream.read((char*)masks, sizeof(masks));
            if (!iStream) {
                throw ImageLoadError{"Failed to read BMP color masks."};
            }

            dib.redMask = reverseEndianness ? swapBytes(masks[0]) : masks[0];
            dib.greenMask = reverseEndianness ? swapBytes(masks[1]) : masks[1];
            dib.blueMask = reverseEndianness ? swapBytes(masks[2]) : masks[2];
            dib.alphaMask = 0; // alphaMask is already zero-initialized, but good to make this explicit here
        } else if (compression == ECompression::AlphaBitfields) {
            tlog::debug()
                << "BMP uses ALPHABITFIELDS compression but DIB header is too small to contain color masks; reading masks from following bytes";

            uint32_t masks[4];
            iStream.read((char*)masks, sizeof(masks));
            if (!iStream) {
                throw ImageLoadError{"Failed to read BMP color masks."};
            }

            dib.redMask = reverseEndianness ? swapBytes(masks[0]) : masks[0];
            dib.greenMask = reverseEndianness ? swapBytes(masks[1]) : masks[1];
            dib.blueMask = reverseEndianness ? swapBytes(masks[2]) : masks[2];
            dib.alphaMask = reverseEndianness ? swapBytes(masks[3]) : masks[3];
        } else {
            setColorMasksToDefault();
        }
    }

    // Ideally, we would leave the masks intact if the header was large enough to contain them, regardless of bitfields setting, but in
    // practice many BMP files have garbage values in the mask fields in those cases (except for the alpha mask, which is always valid).
    if (compression != ECompression::Bitfields && compression != ECompression::AlphaBitfields) {
        setColorMasksToDefault();
    }

    const size_t redBits = popcount(dib.redMask);
    const size_t greenBits = popcount(dib.greenMask);
    const size_t blueBits = popcount(dib.blueMask);
    const size_t alphaBits = popcount(dib.alphaMask);

    const size_t redMax = redBits > 0 ? (1 << redBits) - 1 : 0;
    const size_t greenMax = greenBits > 0 ? (1 << greenBits) - 1 : 0;
    const size_t blueMax = blueBits > 0 ? (1 << blueBits) - 1 : 0;
    const size_t alphaMax = alphaBits > 0 ? (1 << alphaBits) - 1 : 0;

    const size_t redShift = countr_zero(dib.redMask);
    const size_t greenShift = countr_zero(dib.greenMask);
    const size_t blueShift = countr_zero(dib.blueMask);
    const size_t alphaShift = countr_zero(dib.alphaMask);

    // 64-bit bmps are non-standard and don't work in conjunction with the color masks. But Windows/GDI+ supports them, so we handle them
    // here as well. See https://rupertwh.github.io/bmplib/
    const auto scale = dib.bitsPerPixel == 64 ? Vector4f{1.0f / (float)(1 << 13)} :
                                                Vector4f{
                                                    redMax > 0 ? 1.0f / redMax : 0.0f,
                                                    greenMax > 0 ? 1.0f / greenMax : 0.0f,
                                                    blueMax > 0 ? 1.0f / blueMax : 0.0f,
                                                    alphaMax > 0 ? 1.0f / alphaMax : 0.0f,
                                                };

    enum class EColorSpace : uint32_t {
        CalibratedRgb = 0,
        Srgb = fourcc("sRGB"),
        Windows = fourcc("Win "),
        IccEmbedded = fourcc("MBED"),
        IccLinked = fourcc("LINK"),
    };
    const auto cs = (EColorSpace)dib.colorSpaceType;

    HeapArray<uint8_t> iccProfileData;
    switch (cs) {
        case EColorSpace::CalibratedRgb:
        case EColorSpace::Srgb: break;
        case EColorSpace::Windows: break;
        case EColorSpace::IccEmbedded: {
            if (dib.iccProfileSize == 0 || dib.iccProfileData == 0) {
                tlog::warning() << "BMP indicates embedded ICC profile but profile info is missing; skipping ICC profile handling";
                break;
            }

            iccProfileData.resize(dib.iccProfileSize);
            iStream.seekg(dibHeaderBegin + dib.iccProfileData, ios_base::beg);
            iStream.read((char*)iccProfileData.data(), iccProfileData.size());
            if (!iStream) {
                iccProfileData = {};
                tlog::warning() << fmt::format("Failed to read ICC profile data of size {}", iccProfileData.size());
                break;
            }
        } break;
        case EColorSpace::IccLinked: {
            if (dib.iccProfileSize == 0 || dib.iccProfileData == 0) {
                tlog::warning() << "BMP indicates linked ICC profile but profile info is missing; skipping ICC profile handling";
                break;
            }

            iStream.seekg(dibHeaderBegin + dib.iccProfileData, ios_base::beg);
            string path(dib.iccProfileSize, '\0');
            iStream.read(path.data(), dib.iccProfileSize);
            if (!iStream) {
                tlog::warning() << fmt::format("Failed to read ICC profile path of size {}", iccProfileData.size());
                break;
            }

            tlog::warning()
                << fmt::format("Image contains path to an ICC profile '{}' but tev will not attempt read it for security concerns", path);
        } break;
        default: tlog::warning() << fmt::format("Unsupported BMP color space type {:08X}, assuming sRGB", dib.colorSpaceType); break;
    }

    optional<Vector3f> gamma = nullopt;
    optional<chroma_t> chroma = nullopt;

    // Color space precedence: if there's an ICC profile, use it. Otherwise, if it's sRGB or Windows, assume sRGB. Otherwise, if it's
    // calibrated RGB, use the gamma and chroma info in the header.
    if (cs == EColorSpace::CalibratedRgb) {
        gamma = Vector3f{
            (float)dib.gammaRed / (float)(1 << 16),
            (float)dib.gammaGreen / (float)(1 << 16),
            (float)dib.gammaBlue / (float)(1 << 16),
        };

        const auto R = Vector3f{
            (float)dib.redX / (float)(1 << 30),
            (float)dib.redY / (float)(1 << 30),
            (float)dib.redZ / (float)(1 << 30),
        };

        const auto G = Vector3f{
            (float)dib.greenX / (float)(1 << 30),
            (float)dib.greenY / (float)(1 << 30),
            (float)dib.greenZ / (float)(1 << 30),
        };

        const auto B = Vector3f{
            (float)dib.blueX / (float)(1 << 30),
            (float)dib.blueY / (float)(1 << 30),
            (float)dib.blueZ / (float)(1 << 30),
        };

        const float rs = R.x() + R.y() + R.z();
        const float gs = G.x() + G.y() + G.z();
        const float bs = B.x() + B.y() + B.z();

        if (rs > 0.0f && gs > 0.0f && bs > 0.0f) {
            chroma = chroma_t{
                {
                 {R.x() / rs, R.y() / rs},
                 {G.x() / gs, G.y() / gs},
                 {B.x() / bs, B.y() / bs},
                 whiteD65(),
                 }
            };
        }
    }

    const auto convertIntent = [](uint32_t dibIntent) -> optional<ERenderingIntent> {
        switch (dibIntent) {
            case 0x00000000: return nullopt;
            case 0x00000001: return ERenderingIntent::Saturation;
            case 0x00000002: return ERenderingIntent::RelativeColorimetric;
            case 0x00000004: return ERenderingIntent::Perceptual;
            case 0x00000008: return ERenderingIntent::AbsoluteColorimetric;
            default:
                tlog::warning() << fmt::format("Unknown BMP rendering intent: {:08X}; ignoring rendering intent", dibIntent);
                return nullopt;
        }
    };

    const auto renderingIntent = convertIntent(dib.intent);

    const bool flipVertically = dib.height > 0;
    dib.height = abs(dib.height);

    if (dib.height <= 0 || dib.width <= 0) {
        throw ImageLoadError{fmt::format("Invalid BMP image dimensions: {}x{}", dib.width, dib.height)};
    }

    HeapArray<uint8_t> palette; // RRGGBBAA per element
    const size_t paletteEntrySize = type == EType::Os2V1 ? 3 : 4;
    if (hasPalette) {
        const size_t numPaletteEntries = dib.colorsUsed > 0 ? dib.colorsUsed : (1 << dib.bitsPerPixel);
        palette.resize(numPaletteEntries * paletteEntrySize);

        iStream.read((char*)palette.data(), palette.size());
        if (!iStream) {
            throw ImageLoadError{
                fmt::format("Failed to read BMP palette with {} entries and entry size {}", numPaletteEntries, paletteEntrySize)
            };
        }

        if (palette.size() == 0) {
            throw ImageLoadError{"BMP palette is empty."};
        }
    }

    tlog::debug() << fmt::format(
        "BMP info({}): size={} planes={} bpp={} compression={} palette={} redMask={:08X} greenMask={:08X} blueMask={:08X} alphaMask={:08X} colorSpaceType={:08X}",
        dib.size,
        Vector2i{dib.width, dib.height},
        dib.planes,
        dib.bitsPerPixel,
        compressionToString(compression),
        palette.size() / paletteEntrySize,
        dib.redMask,
        dib.greenMask,
        dib.blueMask,
        dib.alphaMask,
        dib.colorSpaceType
    );

    if (dib.size >= 108) {
        tlog::debug() << fmt::format(
            "BMP DIB header v4 color space: gamma={} chroma={}",
            gamma.has_value() ? fmt::format("{}", *gamma) : "n/a",
            chroma.has_value() ? fmt::format("{}", *chroma) : "n/a"
        );
    }

    if (dib.size >= 124) {
        tlog::debug() << fmt::format(
            "BMP DIB header v5 color profile: intent={} profileSize={}",
            renderingIntent.has_value() ? toString(*renderingIntent) : "n/a",
            dib.iccProfileSize
        );
    }

    const size_t bytesPerRow = nextMultiple(dib.bitsPerPixel * dib.width, 32) / 8; // Rows are padded to a multiple of 4 bytes
    const size_t pixelDataSize = bytesPerRow * abs(dib.height);

    if (pixelDataOffset.has_value()) {
        iStream.seekg(*pixelDataOffset, ios_base::beg);
    }

    if (!iStream) {
        throw ImageLoadError{"Failed to seek to BMP pixel data."};
    }

    const size_t pixelDataPos = iStream.tellg();
    iStream.seekg(0, ios::end);
    const size_t pixelDataEnd = iStream.tellg();
    iStream.seekg(pixelDataPos, ios_base::beg);

    const bool isCompressed = compression == ECompression::Rle8 || compression == ECompression::Rle4 ||
        compression == ECompression::Rle24 || compression == ECompression::Huffman;

    bytesToRead = isCompressed ? dib.imageSize : pixelDataSize;
    if (pixelDataEnd - pixelDataPos < bytesToRead) {
        throw ImageLoadError{fmt::format(
            "BMP file is too small to contain expected pixel data: {} bytes available, {} bytes expected", pixelDataEnd - pixelDataPos, pixelDataSize
        )};
    }

    HeapArray<uint8_t> pixelData(bytesToRead);
    iStream.read((char*)pixelData.data(), pixelData.size());
    if (!iStream) {
        throw ImageLoadError{fmt::format("Failed to read BMP pixel data of size {}", pixelData.size())};
    }

    switch (compression) {
        case ECompression::Rle8: pixelData = decode_rle8(pixelData.data(), pixelData.size(), dib.width, dib.height); break;
        case ECompression::Rle4: pixelData = decode_rle4(pixelData.data(), pixelData.size(), dib.width, dib.height); break;
        case ECompression::Rle24: pixelData = decode_rle24(pixelData.data(), pixelData.size(), dib.width, dib.height); break;
        case ECompression::Huffman: pixelData = decode_huffman1d(pixelData.data(), pixelData.size(), dib.width, dib.height); break;
        default: break;
    }

    if (pixelData.size() < pixelDataSize) {
        throw ImageLoadError{fmt::format("Decoded BMP pixel data size {} is smaller than expected {}", pixelData.size(), pixelDataSize)};
    }

    const auto colorBpp = hasPalette ? paletteEntrySize * 8 : dib.bitsPerPixel;

    const size_t numColorChannels = 3;

    // 24 bpp images have no alpha channel, but all others can have one, depending on the color masks.
    const bool hasAlpha = colorBpp != 24;
    const size_t numChannels = numColorChannels + (hasAlpha ? 1 : 0);
    const size_t numPixels = (size_t)dib.width * dib.height;

    const auto size = Vector2i{dib.width, dib.height};

    vector<ImageData> result(1);
    ImageData& resultData = result[0];

    resultData.channels = co_await makeRgbaInterleavedChannels(
        numChannels, 4, hasAlpha, size, EPixelFormat::F32, EPixelFormat::F16, resultData.partName, priority
    );
    resultData.hasPremultipliedAlpha = !hasAlpha;

    atomic<bool> allTransparent = hasAlpha;

    HeapArray<float> floatData = {(size_t)numPixels * numChannels};
    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        size.y(),
        numPixels * numChannels,
        [&](int y) {
            const size_t rowStart = y * bytesPerRow;

            for (int x = 0; x < size.x(); ++x) {
                const size_t outputY = flipVertically ? dib.height - 1 - y : y;

                int32_t rgba[4] = {};
                auto& [r, g, b, a] = rgba;
                a = alphaMax;

                const size_t pixelByte = rowStart + (size_t)x * dib.bitsPerPixel / 8;

                uint8_t* pixelPtr = pixelData.data() + pixelByte;

                if (hasPalette) {
                    const size_t pixelBit = (size_t)x * dib.bitsPerPixel;
                    const size_t pixelByte = pixelBit / 8;
                    const size_t pixelBitOffset = pixelBit - pixelByte * 8;
                    const size_t byte = pixelData[rowStart + pixelByte];
                    const size_t paletteIdx = (byte >> (8 - pixelBitOffset - dib.bitsPerPixel)) & ((1 << dib.bitsPerPixel) - 1);

                    const auto paletteByteIdx = std::min(paletteIdx * paletteEntrySize, palette.size() - paletteEntrySize);

                    pixelPtr = palette.data() + paletteByteIdx;
                }

                switch (colorBpp) {
                    case 16: {
                        const auto pixelValue = read<uint16_t>(pixelPtr, reverseEndianness);
                        r = dib.redMask ? (pixelValue & dib.redMask) >> redShift : 0;
                        g = dib.greenMask ? (pixelValue & dib.greenMask) >> greenShift : 0;
                        b = dib.blueMask ? (pixelValue & dib.blueMask) >> blueShift : 0;
                        a = dib.alphaMask ? (pixelValue & dib.alphaMask) >> alphaShift : 0;
                        break;
                    }
                    case 24: {
                        b = pixelPtr[0];
                        g = pixelPtr[1];
                        r = pixelPtr[2];
                        break;
                    }
                    case 32: {
                        const auto pixelValue = read<uint32_t>(pixelPtr, reverseEndianness);
                        r = dib.redMask ? (pixelValue & dib.redMask) >> redShift : 0;
                        g = dib.greenMask ? (pixelValue & dib.greenMask) >> greenShift : 0;
                        b = dib.blueMask ? (pixelValue & dib.blueMask) >> blueShift : 0;
                        a = dib.alphaMask ? (pixelValue & dib.alphaMask) >> alphaShift : 0;
                        break;
                    }
                    case 64: {
                        // See https://rupertwh.github.io/bmplib/
                        b = read<int16_t>(pixelPtr + 0, reverseEndianness);
                        g = read<int16_t>(pixelPtr + 2, reverseEndianness);
                        r = read<int16_t>(pixelPtr + 4, reverseEndianness);
                        a = read<int16_t>(pixelPtr + 6, reverseEndianness);
                        break;
                    }
                    default: throw ImageLoadError{fmt::format("Unsupported BMP bits per pixel: {}", colorBpp)};
                }

                const size_t idx = (size_t)outputY * dib.width + x;
                for (size_t c = 0; c < 3; ++c) {
                    floatData[idx * numChannels + c] = scale[c] == 0.0f ? 0.0f : (rgba[c] * scale[c]);
                }

                if (hasAlpha) {
                    const float alpha = scale[3] == 0.0f ? 1.0f : (rgba[3] * scale[3]);
                    floatData[idx * numChannels + 3] = alpha;

                    if (alpha != 0.0f) {
                        allTransparent.store(false, memory_order_relaxed);
                    }
                }
            }
        },
        priority
    );

    if (allTransparent) {
        tlog::debug() << "BMP image is fully transparent; flipping to all opaque";
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0, numPixels, numPixels * 4, [&](size_t i) { floatData[i * 4 + 3] = 1.0f; }, priority
        );
    }

    float* const dstData = resultData.channels.front().floatData();
    if (iccProfileData) {
        try {
            const auto profile = ColorProfile::fromIcc(iccProfileData);
            co_await toLinearSrgbPremul(
                profile,
                size,
                numColorChannels,
                numChannels > numColorChannels ? EAlphaKind::Straight : EAlphaKind::None,
                EPixelFormat::F32,
                (uint8_t*)floatData.data(),
                dstData,
                4,
                renderingIntent,
                priority
            );

            resultData.hasPremultipliedAlpha = true;
            resultData.readMetadataFromIcc(profile);
            resultData.renderingIntent = renderingIntent.value_or(resultData.renderingIntent);
            co_return result;
        } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
    }

    resultData.renderingIntent = renderingIntent.value_or(ERenderingIntent::RelativeColorimetric);

    if (chroma.has_value()) {
        resultData.toRec709 = convertColorspaceMatrix(*chroma, rec709Chroma(), resultData.renderingIntent);
        resultData.nativeMetadata.chroma = chroma;
    } else {
        resultData.nativeMetadata.chroma = rec709Chroma();
    }

    const auto effectiveGamma = gamma.value_or(Vector3f{0.0f});
    if (effectiveGamma != Vector3f{0.0f}) {
        resultData.nativeMetadata.gamma = effectiveGamma.x();
        resultData.nativeMetadata.transfer = ituth273::ETransfer::GenericGamma;
    } else {
        resultData.nativeMetadata.transfer = ituth273::ETransfer::SRGB;
    }

    // Non-ICC color space handling
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        numPixels,
        numPixels * 4,
        [&](size_t i) {
            for (size_t c = 0; c < numChannels; ++c) {
                float& dst = dstData[i * 4 + c];
                dst = floatData[i * numChannels + c];

                const bool invertTransfer = c < 3 && dib.bitsPerPixel != 64;

                // 64bpp seems to be an HDR format with linear channels
                if (invertTransfer) {
                    const float g = c < 3 ? effectiveGamma[c] : 0.0f;

                    // Modern browsers / image viewers treat untagged BMPs as sRGB, so we do the same. But if a gamma is explicitly
                    // specified in the header, we respect that instead.
                    if (g > 0.0f) {
                        dst = powf(dst, g);
                    } else {
                        dst = toLinear(dst);
                    }
                }
            }
        },
        priority
    );

    co_return result;
}

} // namespace tev
