#include "aibox/event_store.hpp"

#include <sqlite3.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

using namespace aibox;
using namespace std::chrono_literals;

namespace {
std::string scalar_text(sqlite3* db, const char* sql) {
  sqlite3_stmt* statement{};
  assert(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_ROW);
  const auto* value = sqlite3_column_text(statement, 0);
  const std::string result = value ? reinterpret_cast<const char*>(value) : "";
  sqlite3_finalize(statement);
  return result;
}

double scalar_double(sqlite3* db, const char* sql) {
  sqlite3_stmt* statement{};
  assert(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_ROW);
  const double result = sqlite3_column_double(statement, 0);
  sqlite3_finalize(statement);
  return result;
}

Event make_event(const std::string& id, Mono t) {
  Event event;
  event.id = id;
  event.type = "loitering";
  event.zone_id = "loading";
  event.session_id = "test-session";
  event.track_id = 11;
  event.started = t;
  event.ended = t;
  event.started_utc = std::chrono::system_clock::now();
  event.ended_utc = event.started_utc;
  event.dwell_seconds = 61.0;
  event.active = true;
  return event;
}

void persists_begin_end_and_recovers_active_rows() {
  const auto path = std::filesystem::temp_directory_path() /
      ("aibox-event-test-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite3");
  const Mono t{};
  {
    EventStore store(path.string());
    auto event = make_event("session-1", t);
    assert(store.enqueue({event, true}));
    event.active = false;
    event.ended = t + 3500ms;
    event.ended_utc = event.started_utc + 3500ms;
    event.reason = "zone_exit";
    assert(store.enqueue({event, false}));
    event = make_event("session-2", t);
    assert(store.enqueue({event, true}));
    store.flush();
    assert(store.healthy());
    const auto rows = store.recent(0, 10);
    assert(rows.size() == 2);
    assert(rows[0].id == "session-2" && rows[0].active);
    assert(rows[1].id == "session-1" && !rows[1].active);
    bool rejected_limit = false;
    try { store.recent(0, 201); }
    catch (const std::invalid_argument&) { rejected_limit = true; }
    assert(rejected_limit);

    sqlite3* db{};
    assert(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    assert(scalar_text(db, "SELECT end_reason FROM events WHERE id='session-1'") == "zone_exit");
    assert(scalar_double(db, "SELECT duration_seconds FROM events WHERE id='session-1'") > 3.49);
    assert(scalar_double(db, "SELECT active FROM events WHERE id='session-1'") == 0);
    assert(scalar_double(db, "SELECT active FROM events WHERE id='session-2'") == 1);
    sqlite3_close(db);
  }
  {
    EventStore recovered(path.string());
    recovered.flush();
    sqlite3* db{};
    assert(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    assert(scalar_text(db, "SELECT end_reason FROM events WHERE id='session-2'") == "process_restarted");
    assert(scalar_double(db, "SELECT active FROM events WHERE id='session-2'") == 0);
    sqlite3_close(db);
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-wal");
  std::filesystem::remove(path.string() + "-shm");
}
}  // namespace

int main() { persists_begin_end_and_recovers_active_rows(); }
