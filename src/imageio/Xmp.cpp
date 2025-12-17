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
#include <tev/imageio/Xmp.h>

#define TXMP_STRING_TYPE std::string
#include <XMP.hpp>
#include <XMP.incl_cpp>

#include <algorithm>
#include <string>
#include <string_view>

using namespace std;

namespace tev {

class XMPContext {
public:
    static bool init() {
        std::call_once(initFlag, []() {
            initialized = SXMPMeta::Initialize();
            if (initialized) {
                std::atexit(shutdown);
            }
        });

        return initialized;
    }

private:
    static void shutdown() {
        if (initialized) {
            SXMPMeta::Terminate();
        }
    }

    static std::once_flag initFlag;
    static bool initialized;
};

std::once_flag XMPContext::initFlag;
bool XMPContext::initialized = false;

Xmp::Xmp(string_view xmpData) {
    if (!XMPContext::init()) {
        throw invalid_argument{"Failed to initialize XMP toolkit."};
    }

    mAttributes.name = "XMP";

    try {
        SXMPMeta meta;
        meta.ParseFromBuffer(xmpData.data(), xmpData.size());

        // tlog::debug() << xmpData;

        SXMPIterator iter{meta};
        string schema, path, value;

        while (iter.Next(&schema, &path, &value)) {
            // tlog::debug() << fmt::format("{} | {} | {}", schema, path, value);

            if (value.empty()) {
                continue;
            }

            AttributeNode* node = &mAttributes;
            const auto parts = split(path, ":/", true);

            for (const auto& part : parts) {
                // Search from the back because XMP properties are often nested in order.
                const auto it = std::find_if(node->children.rbegin(), node->children.rend(), [&](const auto& child) {
                    return child.name == part;
                });

                if (it == node->children.rend()) {
                    node->children.emplace_back(AttributeNode{.name = string{part}, .value = "", .type = "", .children = {}});
                    node = &node->children.back();
                    continue;
                }

                node = &(*it);
            }

            if (!node->value.empty()) {
                tlog::warning()
                    << fmt::format("XMP property '{}' already has a value '{}', overwriting with new value '{}'.", path, node->value, value);
            }

            node->value = value;
            node->type = "string";
        }

        if (string orientationStr; meta.GetProperty(kXMP_NS_TIFF, "Orientation", &orientationStr, nullptr)) {
            try {
                const int orientationInt = stoi(orientationStr);
                mOrientation = static_cast<EOrientation>(orientationInt);
                tlog::debug() << fmt::format("Found XMP orientation: {}", orientationInt);
            } catch (const invalid_argument&) {
                tlog::warning() << fmt::format("Failed to parse XMP orientation value: '{}'.", orientationStr);
            }
        }
    } catch (XMP_Error& e) { throw invalid_argument{e.GetErrMsg()}; }
}

} // namespace tev
