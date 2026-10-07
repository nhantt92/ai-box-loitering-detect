#include "aibox/event_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <condition_variable>
#include <array>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <sys/stat.h>

namespace aibox {
namespace {

void check_sqlite(int code, sqlite3* db, const char* operation) {
  if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
    throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(db));
}

void exec(sqlite3* db, const char* sql) {
  char* message = nullptr;
  const int code = sqlite3_exec(db, sql, nullptr, nullptr, &message);
  if (code != SQLITE_OK) {
    const std::string error = message ? message : sqlite3_errmsg(db);
    sqlite3_free(message);
    throw std::runtime_error("SQLite: " + error);
  }
}

std::string utc_iso(std::chrono::system_clock::time_point at) {
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(at);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(at - seconds).count();
  const std::time_t value = std::chrono::system_clock::to_time_t(seconds);
  std::tm tm{};
  gmtime_r(&value, &tm);
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.'
      << std::setw(3) << std::setfill('0') << millis << 'Z';
  return out.str();
}

class Statement {
 public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    check_sqlite(sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr), db, "prepare");
  }
  ~Statement() { sqlite3_finalize(statement_); }
  sqlite3_stmt* get() const { return statement_; }
 private:
  sqlite3* db_;
  sqlite3_stmt* statement_{};
};

void bind_text(sqlite3_stmt* statement, int index, const std::string& value) {
  check_sqlite(sqlite3_bind_text(statement, index, value.c_str(),
                                 static_cast<int>(value.size()), SQLITE_TRANSIENT),
               sqlite3_db_handle(statement), "bind text");
}

bool has_column(sqlite3* db, const char* table, const char* column) {
  const std::string sql = std::string("PRAGMA table_info(") + table + ")";
  Statement statement(db, sql.c_str());
  for (;;) {
    const int code = sqlite3_step(statement.get());
    if (code == SQLITE_DONE) return false;
    check_sqlite(code, db, "inspect table columns");
    const auto* name = sqlite3_column_text(statement.get(), 1);
    if (name && std::string(reinterpret_cast<const char*>(name)) == column)
      return true;
  }
}

}  // namespace

struct EventStore::Impl {
  struct WorkItem {
    enum class Kind { Transition, SnapshotState, DeleteEvent } kind{Kind::Transition};
    EventTransition transition;
    std::string event_id;
    std::string snapshot_state;
    std::string snapshot_path;
  };

  Impl(std::string path, std::size_t capacity)
      : database_path(std::move(path)), queue_capacity(capacity) {
    if (database_path.empty() || !queue_capacity)
      throw std::invalid_argument("EventStore path and queue capacity must be valid");
    const std::filesystem::path file(database_path);
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path());
    const int open_code = sqlite3_open_v2(database_path.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (open_code != SQLITE_OK) {
      const std::string error = db_ ? sqlite3_errmsg(db_) : "sqlite open failed";
      if (db_) sqlite3_close(db_);
      db_ = nullptr;
      throw std::runtime_error(error);
    }
    try {
      if (chmod(database_path.c_str(), S_IRUSR | S_IWUSR) != 0)
        throw std::runtime_error("Cannot restrict permissions on event database");
      sqlite3_busy_timeout(db_, 5000);
      exec(db_, "PRAGMA journal_mode=WAL;");
      exec(db_, "PRAGMA synchronous=NORMAL;");
      chmod((database_path + "-wal").c_str(), S_IRUSR | S_IWUSR);
      chmod((database_path + "-shm").c_str(), S_IRUSR | S_IWUSR);
      exec(db_, "CREATE TABLE IF NOT EXISTS events ("
              "id TEXT PRIMARY KEY, session_id TEXT NOT NULL, track_id INTEGER NOT NULL,"
              "crowd_count INTEGER NOT NULL DEFAULT 0,crowd_threshold INTEGER NOT NULL DEFAULT 0,"
              "type TEXT NOT NULL, zone_id TEXT NOT NULL DEFAULT '',"
              "started_utc TEXT NOT NULL, ended_utc TEXT, duration_seconds REAL NOT NULL DEFAULT 0,"
              "dwell_seconds REAL NOT NULL DEFAULT 0, peak_speed REAL NOT NULL DEFAULT 0,"
              "speed_unit TEXT NOT NULL DEFAULT '', end_reason TEXT NOT NULL DEFAULT '',"
              "active INTEGER NOT NULL, snapshot_state TEXT NOT NULL DEFAULT 'none',"
              "snapshot_path TEXT NOT NULL DEFAULT '');"
              "CREATE INDEX IF NOT EXISTS events_started_idx ON events(started_utc DESC);");
      if (!has_column(db_, "events", "crowd_count"))
        exec(db_, "ALTER TABLE events ADD COLUMN crowd_count INTEGER NOT NULL DEFAULT 0");
      if (!has_column(db_, "events", "crowd_threshold"))
        exec(db_, "ALTER TABLE events ADD COLUMN crowd_threshold INTEGER NOT NULL DEFAULT 0");
      // An active row cannot remain active after the process has restarted.
      Statement recover(db_, "UPDATE events SET active=0, ended_utc=?,"
                           "end_reason='process_restarted',"
                           "duration_seconds=MAX(0,(julianday(?) - julianday(started_utc))*86400)"
                           " WHERE active=1");
      const auto now = utc_iso(std::chrono::system_clock::now());
      bind_text(recover.get(), 1, now);
      bind_text(recover.get(), 2, now);
      check_sqlite(sqlite3_step(recover.get()), db_, "recover active events");
      worker = std::thread([this] { run(); });
    } catch (...) {
      if (worker.joinable()) {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        ready.notify_all();
        worker.join();
      }
      if (db_) sqlite3_close(db_);
      db_ = nullptr;
      throw;
    }
  }

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    ready.notify_all();
    space.notify_all();
    if (worker.joinable()) worker.join();
    if (db_) sqlite3_close(db_);
  }

  void persist_transition(const EventTransition& transition) {
    std::lock_guard<std::mutex> database_lock(database_mutex);
    const Event& event = transition.event;
    if (transition.started) {
      Statement statement(db_, "INSERT INTO events(id,session_id,track_id,type,zone_id,"
          "started_utc,dwell_seconds,peak_speed,speed_unit,active,snapshot_state,"
          "crowd_count,crowd_threshold)"
          " VALUES(?,?,?,?,?,?,?,?,?,1,'none',?,?) ON CONFLICT(id) DO NOTHING");
      bind_text(statement.get(), 1, event.id);
      bind_text(statement.get(), 2, event.session_id);
      check_sqlite(sqlite3_bind_int(statement.get(), 3, event.track_id), db_, "bind track");
      bind_text(statement.get(), 4, event.type);
      bind_text(statement.get(), 5, event.zone_id);
      bind_text(statement.get(), 6, utc_iso(event.started_utc));
      check_sqlite(sqlite3_bind_double(statement.get(), 7, event.dwell_seconds), db_, "bind dwell");
      check_sqlite(sqlite3_bind_double(statement.get(), 8, event.peak_speed), db_, "bind peak speed");
      bind_text(statement.get(), 9, event.speed_unit);
      check_sqlite(sqlite3_bind_int(statement.get(), 10, event.crowd_count), db_, "bind crowd count");
      check_sqlite(sqlite3_bind_int(statement.get(), 11, event.crowd_threshold), db_, "bind crowd threshold");
      check_sqlite(sqlite3_step(statement.get()), db_, "insert event");
    } else {
      const double duration = std::max(0.0,
          std::chrono::duration<double>(event.ended - event.started).count());
      Statement statement(db_, "UPDATE events SET ended_utc=?,duration_seconds=?,"
          "dwell_seconds=?,peak_speed=?,speed_unit=?,end_reason=?,active=0,"
          "crowd_count=?,crowd_threshold=? WHERE id=?");
      bind_text(statement.get(), 1, utc_iso(event.ended_utc));
      check_sqlite(sqlite3_bind_double(statement.get(), 2, duration), db_, "bind duration");
      check_sqlite(sqlite3_bind_double(statement.get(), 3, event.dwell_seconds), db_, "bind dwell");
      check_sqlite(sqlite3_bind_double(statement.get(), 4, event.peak_speed), db_, "bind peak speed");
      bind_text(statement.get(), 5, event.speed_unit);
      bind_text(statement.get(), 6, event.reason);
      check_sqlite(sqlite3_bind_int(statement.get(), 7, event.crowd_count), db_, "bind crowd count");
      check_sqlite(sqlite3_bind_int(statement.get(), 8, event.crowd_threshold), db_, "bind crowd threshold");
      bind_text(statement.get(), 9, event.id);
      check_sqlite(sqlite3_step(statement.get()), db_, "end event");
      if (sqlite3_changes(db_) == 0)
        throw std::runtime_error("SQLite event end has no matching begin row: " + event.id);
    }
  }

  void persist_snapshot_state(const WorkItem& item) {
    std::lock_guard<std::mutex> database_lock(database_mutex);
    Statement statement(db_, "UPDATE events SET snapshot_state=?,snapshot_path=? WHERE id=?");
    bind_text(statement.get(), 1, item.snapshot_state);
    bind_text(statement.get(), 2, item.snapshot_path);
    bind_text(statement.get(), 3, item.event_id);
    check_sqlite(sqlite3_step(statement.get()), db_, "update event snapshot state");
    if (sqlite3_changes(db_) == 0)
      throw std::runtime_error("Snapshot update has no matching event: " + item.event_id);
  }

  void persist_delete_event(const WorkItem& item) {
    std::lock_guard<std::mutex> database_lock(database_mutex);
    Statement statement(db_, "DELETE FROM events WHERE id=?");
    bind_text(statement.get(), 1, item.event_id);
    check_sqlite(sqlite3_step(statement.get()), db_, "delete retained event");
  }

  void run() {
    for (;;) {
      WorkItem item;
      {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [&] { return stopping || !queue.empty(); });
        if (queue.empty() && stopping) break;
        item = std::move(queue.front());
        queue.pop_front();
        writing = true;
        space.notify_all();
      }
      try {
        switch (item.kind) {
          case WorkItem::Kind::Transition:
            persist_transition(item.transition);
            break;
          case WorkItem::Kind::SnapshotState:
            persist_snapshot_state(item);
            break;
          case WorkItem::Kind::DeleteEvent:
            persist_delete_event(item);
            break;
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          error_message.clear();
        }
      } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex);
        error_message = error.what();
        std::cerr << "Event storage error: " << error_message << '\n';
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        writing = false;
        if (queue.empty()) drained.notify_all();
      }
    }
    if (db_) sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
  }

  std::string database_path;
  std::size_t queue_capacity;
  sqlite3* db_{};
  mutable std::mutex database_mutex;
  mutable std::mutex mutex;
  std::condition_variable ready, space, drained;
  std::deque<WorkItem> queue;
  std::thread worker;
  bool stopping{}, writing{};
  std::string error_message;
};

EventStore::EventStore(std::string path, std::size_t capacity)
    : impl_(std::make_unique<Impl>(std::move(path), capacity)) {}
EventStore::~EventStore() = default;

bool EventStore::enqueue(EventTransition transition) {
  auto& p = *impl_;
  std::unique_lock<std::mutex> lock(p.mutex);
  const bool room = p.space.wait_for(lock, std::chrono::milliseconds(100), [&] {
    return p.stopping || p.queue.size() < p.queue_capacity;
  });
  if (!room) {
    p.error_message = "event writer queue remained full for 100 ms";
    return false;
  }
  if (p.stopping) return false;
  Impl::WorkItem item;
  item.kind = Impl::WorkItem::Kind::Transition;
  item.transition = std::move(transition);
  p.queue.push_back(std::move(item));
  if (p.error_message == "event writer queue remained full for 100 ms")
    p.error_message.clear();
  p.ready.notify_one();
  return true;
}

bool EventStore::enqueue_snapshot_state(std::string event_id, std::string state,
                                       std::string relative_path) {
  static const std::array<const char*, 6> states{
      "pending", "ready", "failed", "deleting", "expiring", "expired"};
  if (event_id.empty() || relative_path.size() > 512 ||
      std::find_if(states.begin(), states.end(), [&](const char* value) {
        return state == value;
      }) == states.end())
    return false;
  auto& p = *impl_;
  std::unique_lock<std::mutex> lock(p.mutex);
  const bool room = p.space.wait_for(lock, std::chrono::milliseconds(100), [&] {
    return p.stopping || p.queue.size() < p.queue_capacity;
  });
  if (!room || p.stopping) return false;
  Impl::WorkItem item;
  item.kind = Impl::WorkItem::Kind::SnapshotState;
  item.event_id = std::move(event_id);
  item.snapshot_state = std::move(state);
  item.snapshot_path = std::move(relative_path);
  p.queue.push_back(std::move(item));
  p.ready.notify_one();
  return true;
}

bool EventStore::enqueue_delete_event(std::string event_id) {
  if (event_id.empty()) return false;
  auto& p = *impl_;
  std::unique_lock<std::mutex> lock(p.mutex);
  const bool room = p.space.wait_for(lock, std::chrono::milliseconds(100), [&] {
    return p.stopping || p.queue.size() < p.queue_capacity;
  });
  if (!room || p.stopping) return false;
  Impl::WorkItem item;
  item.kind = Impl::WorkItem::Kind::DeleteEvent;
  item.event_id = std::move(event_id);
  p.queue.push_back(std::move(item));
  p.ready.notify_one();
  return true;
}

void EventStore::flush() {
  auto& p = *impl_;
  std::unique_lock<std::mutex> lock(p.mutex);
  p.drained.wait(lock, [&] { return p.queue.empty() && !p.writing; });
}

std::vector<StoredEvent> EventStore::recent(std::size_t offset,
                                           std::size_t limit) const {
  if (!limit || limit > 200 || offset > 1000000)
    throw std::invalid_argument("Event query pagination is outside limits");
  auto& p = *impl_;
  std::lock_guard<std::mutex> lock(p.database_mutex);
  Statement query(p.db_, "SELECT id,session_id,track_id,type,zone_id,started_utc,"
      "ended_utc,duration_seconds,dwell_seconds,peak_speed,speed_unit,end_reason,"
      "active,snapshot_state,snapshot_path,crowd_count,crowd_threshold FROM events "
      "ORDER BY started_utc DESC,id DESC LIMIT ? OFFSET ?");
  check_sqlite(sqlite3_bind_int64(query.get(), 1, static_cast<sqlite3_int64>(limit)),
               p.db_, "bind event limit");
  check_sqlite(sqlite3_bind_int64(query.get(), 2, static_cast<sqlite3_int64>(offset)),
               p.db_, "bind event offset");
  std::vector<StoredEvent> rows;
  for (;;) {
    const int code = sqlite3_step(query.get());
    if (code == SQLITE_DONE) break;
    check_sqlite(code, p.db_, "read events");
    auto text = [&](int column) {
      const auto* value = sqlite3_column_text(query.get(), column);
      return value ? std::string(reinterpret_cast<const char*>(value)) : std::string{};
    };
    rows.push_back({text(0), text(1), text(3), text(4), text(5), text(6),
        text(10), text(11), text(13), text(14),
        sqlite3_column_int(query.get(), 2), sqlite3_column_double(query.get(), 7),
        sqlite3_column_double(query.get(), 8), sqlite3_column_double(query.get(), 9),
        sqlite3_column_int(query.get(), 12) != 0,
        sqlite3_column_int(query.get(), 15), sqlite3_column_int(query.get(), 16)});
  }
  return rows;
}

namespace {
StoredEvent read_stored_event(sqlite3_stmt* statement) {
  auto text = [&](int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? std::string(reinterpret_cast<const char*>(value)) : std::string{};
  };
  return {text(0), text(1), text(3), text(4), text(5), text(6),
      text(10), text(11), text(13), text(14), sqlite3_column_int(statement, 2),
      sqlite3_column_double(statement, 7), sqlite3_column_double(statement, 8),
      sqlite3_column_double(statement, 9), sqlite3_column_int(statement, 12) != 0,
      sqlite3_column_int(statement, 15), sqlite3_column_int(statement, 16)};
}
constexpr const char* kEventSelect =
    "SELECT id,session_id,track_id,type,zone_id,started_utc,ended_utc,"
    "duration_seconds,dwell_seconds,peak_speed,speed_unit,end_reason,active,"
    "snapshot_state,snapshot_path,crowd_count,crowd_threshold FROM events ";
}  // namespace

std::optional<StoredEvent> EventStore::get(const std::string& event_id) const {
  if (event_id.empty() || event_id.size() > 128) return std::nullopt;
  auto& p = *impl_;
  std::lock_guard<std::mutex> lock(p.database_mutex);
  const std::string sql = std::string(kEventSelect) + "WHERE id=?";
  Statement query(p.db_, sql.c_str());
  bind_text(query.get(), 1, event_id);
  const int code = sqlite3_step(query.get());
  if (code == SQLITE_DONE) return std::nullopt;
  check_sqlite(code, p.db_, "read event by id");
  return read_stored_event(query.get());
}

std::vector<StoredEvent> EventStore::retention_candidates(std::size_t offset,
                                                          std::size_t limit) const {
  if (!limit || limit > 200 || offset > 100000000)
    throw std::invalid_argument("Event retention pagination is outside limits");
  auto& p = *impl_;
  std::lock_guard<std::mutex> lock(p.database_mutex);
  const std::string sql = std::string(kEventSelect) +
      "ORDER BY started_utc ASC,id ASC LIMIT ? OFFSET ?";
  Statement query(p.db_, sql.c_str());
  check_sqlite(sqlite3_bind_int64(query.get(), 1, static_cast<sqlite3_int64>(limit)),
               p.db_, "bind retention limit");
  check_sqlite(sqlite3_bind_int64(query.get(), 2, static_cast<sqlite3_int64>(offset)),
               p.db_, "bind retention offset");
  std::vector<StoredEvent> rows;
  for (;;) {
    const int code = sqlite3_step(query.get());
    if (code == SQLITE_DONE) break;
    check_sqlite(code, p.db_, "read retention events");
    rows.push_back(read_stored_event(query.get()));
  }
  return rows;
}

bool EventStore::healthy() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->error_message.empty();
}

std::string EventStore::last_error() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->error_message;
}

}  // namespace aibox
