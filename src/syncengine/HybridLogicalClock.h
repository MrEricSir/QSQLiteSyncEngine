// MIT License
//
// Copyright (c) 2026 Eric Gregory <mrericsir@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <cstdint>
#include <mutex>

namespace syncengine {

/*!
    \class syncengine::HybridLogicalClock
    \inmodule QSQLiteSyncEngine
    \internal
    \brief Hybrid Logical Clock for causal ordering without synchronized clocks.

    Encodes timestamps as a 64-bit value: upper 48 bits for the physical
    component (milliseconds since epoch) and lower 16 bits for a logical
    counter. This guarantees monotonically increasing timestamps even when
    the wall clock goes backward.

    \sa SyncEngine
*/
class HybridLogicalClock {
public:
    explicit HybridLogicalClock();

    /*!
        Generates and returns a new timestamp for a local event.
    */
    uint64_t now();

    /*!
        Updates the clock after receiving \a remoteHlc from another client,
        then returns a new local timestamp that is causally after both the
        local and remote clocks.
    */
    uint64_t receive(uint64_t remoteHlc);

    /*!
        Extracts the physical (milliseconds) component from \a hlc.
    */
    static uint64_t physicalComponent(uint64_t hlc);

    /*!
        Extracts the logical counter from \a hlc.
    */
    static uint16_t logicalComponent(uint64_t hlc);

    /*!
        Packs \a physicalMs and \a logical into a single HLC value.
    */
    static uint64_t pack(uint64_t physicalMs, uint16_t logical);

    /*!
        Returns the current HLC value without advancing the clock.
    */
    uint64_t current() const;

private:
    uint64_t wallClockMs() const;

    mutable std::mutex m_mutex;
    uint64_t m_hlc = 0;
};

} // namespace syncengine
