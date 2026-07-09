#include "session_table.h"

namespace app {

session_table::session_table() noexcept {
    for (auto& s : _slots) {
        s.id           = k_invalid_session;
        s.generation   = 0;
        s.account      = 0;
        s.owner_worker = -1;
        s.state        = session_state::closed;
    }
}

session_record* session_table::slot_of(session_id id) noexcept {
    if (id == k_invalid_session) return nullptr;
    for (auto& s : _slots) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

session_record* session_table::allocate(account_id account) noexcept {
    for (auto& s : _slots) {
        if (s.id != k_invalid_session) continue;   // slot in use
        s.id           = _next_id++;
        s.generation  += 1;                          // fresh generation per reuse
        s.account      = account;
        s.owner_worker = -1;
        s.state        = session_state::accepted;
        ++_count;
        return &s;
    }
    return nullptr;   // table full
}

session_record* session_table::find(session_id id) noexcept {
    return slot_of(id);
}

bool session_table::validate(session_id id, u32 generation) noexcept {
    const session_record* s = slot_of(id);
    return s != nullptr && s->generation == generation;
}

bool session_table::mark_assigning(session_id id, i32 worker) noexcept {
    session_record* s = slot_of(id);
    if (s == nullptr) return false;
    s->owner_worker = worker;
    s->state        = session_state::assigning;
    return true;
}

bool session_table::mark_in_world(session_id id) noexcept {
    session_record* s = slot_of(id);
    if (s == nullptr) return false;
    s->state = session_state::in_world;
    return true;
}

bool session_table::remove(session_id id) noexcept {
    session_record* s = slot_of(id);
    if (s == nullptr) return false;
    s->id           = k_invalid_session;   // slot free; generation preserved
    s->owner_worker = -1;
    s->state        = session_state::closed;
    --_count;
    return true;
}

}  // namespace app
