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

#include <tev/Task.h>

namespace tev {

void waitAll(std::span<Task<void>> futures) {
    std::exception_ptr eptr = {};

    for (auto&& f : futures) {
        try {
            f.get();
        } catch (const std::exception& e) {
            if (eptr) {
                tlog::error() << "Multiple exceptions in waitAll(). Rethrowing first and logging others: " << e.what();
            } else {
                eptr = std::current_exception();
            }
        }
    }

    if (eptr) {
        std::rethrow_exception(eptr);
    }
}

Task<void> awaitAll(std::span<Task<void>> futures) {
    std::exception_ptr eptr = {};

    for (auto&& f : futures) {
        try {
            co_await f;
        } catch (const std::exception& e) {
            if (eptr) {
                tlog::error() << "Multiple exceptions in awaitAll(). Rethrowing first and logging others: " << e.what();
            } else {
                eptr = std::current_exception();
            }
        }
    }

    if (eptr) {
        std::rethrow_exception(eptr);
    }
}

}
