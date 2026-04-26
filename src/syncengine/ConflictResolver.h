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

#include "syncengine/SyncableDatabase.h"

namespace syncengine {

/*!
    \class ConflictResolver
    \inmodule QSQLiteSyncEngine
    \internal
    \brief Last-write-wins conflict resolver using HLC timestamps.

    ConflictResolver provides a SyncableDatabase::ConflictHandler that compares
    the incoming changeset's HLC against per-row HLC values stored in the
    \c _sync_row_hlc shadow table. The change with the higher HLC wins. Equal
    HLCs are broken deterministically by client ID.

    \sa SyncableDatabase, HybridLogicalClock
*/
class ConflictResolver {
public:
    /*!
        Constructs a resolver for a changeset from \a incomingClientId with
        timestamp \a incomingHlc, to be applied against \a db.
    */
    explicit ConflictResolver(SyncableDatabase *db, uint64_t incomingHlc,
                              const QString &incomingClientId);

    /*!
        Returns a ConflictHandler suitable for SyncableDatabase::applyChangeset().
    */
    SyncableDatabase::ConflictHandler handler();

private:
    SyncableDatabase *m_db;
    uint64_t m_incomingHlc;
    QString m_incomingClientId;
};

} // namespace syncengine
