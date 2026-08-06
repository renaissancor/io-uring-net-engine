#pragma once
// app/session_table.h
//
// SessionManager authority table — the acceptor's fixed-capacity map from
// session_id to its record (state + owner + generation). Correctness-first
// prototype per handoff: a flat inline array of config::k_session_capacity slots
// with linear scan on lookup. No std:: container (project no-STL policy); no
// heap. A slot with id == k_invalid_session is free.
//
// Not thread-safe: the table is owned and mutated by the acceptor thread ALONE.
// Other threads never touch it — they observe session state only through mesh
// messages (adopt_session / session_closed). This single-owner rule is what
// lets it stay lock-free.

#include "config.h"
#include "session_id.h"
#include "session_record.h"
#include "../types.h"

namespace app {

class session_table {
    session_record _slots[config::k_session_capacity];
    session_id     _next_id = 1;   // monotonic; 0 stays reserved as invalid
    u32            _count   = 0;

public:
    session_table() noexcept;

    session_table(const session_table&)            = delete;
    session_table& operator=(const session_table&) = delete;
    session_table(session_table&&)                 = delete;
    session_table& operator=(session_table&&)      = delete;

    static constexpr usize capacity() noexcept { return config::k_session_capacity; }
    u32 count() const noexcept { return _count; }

    // Mint a new session in state `accepted`. Assigns a process-unique id and a
    // per-slot generation. Returns the record (owned by the table) or nullptr
    // when full. owner_worker starts at -1 (unassigned).
    session_record* allocate(account_id account) noexcept;

    // Linear-scan lookup by id. Returns nullptr if absent (never allocated,
    // or already removed). O(capacity).
    session_record* find(session_id id) noexcept;

    // True iff a live record with this id exists AND its generation matches —
    // the guard that lets the acceptor discard a message referring to a
    // recycled slot. A closed/absent id fails.
    bool validate(session_id id, u32 generation) noexcept;

    // Record the owning worker and flip accepted -> assigning. Returns false if
    // the id is absent.
    bool mark_assigning(session_id id, i32 worker) noexcept;

    // Flip to in_world (worker confirmed adoption + room entry). False if absent.
    bool mark_in_world(session_id id) noexcept;

    // Free the slot for reuse (state -> closed, id -> invalid). Returns false if
    // the id was not present. Bumps nothing here — the generation advances on
    // the NEXT allocate() into this slot.
    bool remove(session_id id) noexcept;

private:
    session_record* slot_of(session_id id) noexcept;
};

}  // namespace app
