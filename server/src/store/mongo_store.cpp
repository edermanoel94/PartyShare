#include "store/mongo_store.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/oid.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>

#include <dv/logging/logger.hpp>

#include "store/memory_store.hpp"

namespace dv::server::store {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

/// The driver insists on exactly one of these per process, created before any
/// other driver call and destroyed after the last one.
///
/// A function local static is what gives that for free: it is constructed on
/// the first call, once even if two threads arrive together, and destroyed at
/// exit after everything that used it. The alternative, an instance owned by
/// whoever opens the first connection, breaks the moment a second one is
/// opened after the first was closed.
void ensure_driver_instance() {
  static const mongocxx::instance instance{};
  (void)instance;
}

/// Puts the configured timeout into the URI, where the driver reads it from.
///
/// Three timeouts and not one: server selection is how long the driver hunts
/// for a reachable node, and without the other two an established connection
/// that stops answering still blocks for the driver's own defaults, which are
/// measured in tens of seconds. All three matter because these calls are made
/// with the server's lock held.
///
/// An operator who already stated a timeout in the URI keeps theirs.
std::string with_timeout(const std::string& uri, int timeout_ms) {
  if (uri.find("serverSelectionTimeoutMS") != std::string::npos) {
    return uri;
  }

  const std::string milliseconds = std::to_string(timeout_ms);
  const std::string parameters = "serverSelectionTimeoutMS=" + milliseconds +
                                 "&connectTimeoutMS=" + milliseconds +
                                 "&socketTimeoutMS=" + milliseconds;

  if (uri.find('?') != std::string::npos) {
    return uri + "&" + parameters;
  }

  // "mongodb://host?x=1" is not a valid URI: a query has to follow a path,
  // even an empty one. "mongodb://host/?x=1" is.
  const std::size_t scheme_end = uri.find("://");
  const std::size_t host_start = scheme_end == std::string::npos ? 0 : scheme_end + 3;
  const bool has_path = uri.find('/', host_start) != std::string::npos;
  return uri + (has_path ? "?" : "/?") + parameters;
}

Error failure(const std::string& what, const std::string& detail) {
  return Error{.code = "database_error", .message = what + ": " + detail};
}

/// Reads a string field, answering empty when it is absent or of another type.
///
/// Tolerant on purpose. These documents are read back from a database that a
/// person can edit and that an older version of this server may have written,
/// and refusing to start over one unexpected field would be a server that
/// cannot be recovered without a database console.
std::string string_field(const bsoncxx::document::view& document, const char* key) {
  const auto element = document[key];
  if (!element || element.type() != bsoncxx::type::k_string) {
    return {};
  }
  return std::string(element.get_string().value);
}

bool bool_field(const bsoncxx::document::view& document, const char* key) {
  const auto element = document[key];
  return element && element.type() == bsoncxx::type::k_bool && element.get_bool().value;
}

std::int64_t int_field(const bsoncxx::document::view& document, const char* key) {
  const auto element = document[key];
  if (!element) {
    return 0;
  }
  if (element.type() == bsoncxx::type::k_int64) {
    return element.get_int64().value;
  }
  if (element.type() == bsoncxx::type::k_int32) {
    return element.get_int32().value;
  }
  return 0;
}

Account account_from(const bsoncxx::document::view& document) {
  Account account;
  account.username = string_field(document, "username");
  account.salt_hex = string_field(document, "salt_hex");
  account.password_hash_hex = string_field(document, "password_hash_hex");
  account.created_at = int_field(document, "created_at");
  account.user.id = string_field(document, "user_id");
  account.user.display_name = string_field(document, "display_name");
  account.user.avatar = string_field(document, "avatar");
  account.user.role = models::role_from_string(string_field(document, "role"));
  return account;
}

bsoncxx::document::value account_to_document(const Account& account) {
  return make_document(
      kvp("user_id", account.user.id), kvp("username", account.username),
      kvp("display_name", account.user.display_name), kvp("avatar", account.user.avatar),
      kvp("role", std::string(models::to_string(account.user.role))),
      kvp("salt_hex", account.salt_hex), kvp("password_hash_hex", account.password_hash_hex),
      kvp("created_at", account.created_at));
}

RoomRecord room_from(const bsoncxx::document::view& document) {
  RoomRecord record;
  record.id = string_field(document, "id");
  record.name = string_field(document, "name");
  record.owner_id = string_field(document, "owner_id");
  record.persistent = bool_field(document, "persistent");
  record.created_at = int_field(document, "created_at");
  return record;
}

models::AuditEntry audit_from(const bsoncxx::document::view& document) {
  models::AuditEntry entry;
  const auto id = document["_id"];
  if (id && id.type() == bsoncxx::type::k_oid) {
    entry.id = id.get_oid().value.to_string();
  }
  entry.actor_id = string_field(document, "actor_id");
  entry.actor_username = string_field(document, "actor_username");
  entry.action = string_field(document, "action");
  entry.target_id = string_field(document, "target_id");
  entry.room_id = string_field(document, "room_id");
  entry.detail = string_field(document, "detail");
  entry.timestamp_seconds = int_field(document, "timestamp_seconds");
  return entry;
}

/// The connection the three stores share.
///
/// One pool and one database name, held in one place, because the three stores
/// are three views of the same database. The pool is internally synchronized,
/// which is what lets the const reads below acquire from it.
struct Session {
  Session(mongocxx::uri uri, std::string name) : pool(std::move(uri)), database(std::move(name)) {}

  mutable mongocxx::pool pool;
  std::string database;
};

/// Accounts. See UserStore for the contract.
class MongoUserStore final : public UserStore {
 public:
  explicit MongoUserStore(Session& session) : session_(session) {}

  /// Creates the indexes the collection needs. Called once, at open.
  ///
  /// The unique ones are not an optimization, they are the constraint: two
  /// accounts with the same username are a state this server has no way to
  /// recover from, and the database is the only place that can refuse them
  /// under concurrency.
  [[nodiscard]] std::optional<Error> ensure_indexes() {
    try {
      auto client = session_.pool.acquire();
      auto database = (*client)[session_.database];
      database["users"].create_index(make_document(kvp("username", 1)),
                                     make_document(kvp("unique", true)));
      database["users"].create_index(make_document(kvp("user_id", 1)),
                                     make_document(kvp("unique", true)));
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not create the account indexes", error.what());
    }
  }

  [[nodiscard]] std::optional<Error> create(Account account) override {
    if (account.created_at == 0) {
      account.created_at = unix_seconds_now();
    }
    try {
      auto client = session_.pool.acquire();
      auto users = (*client)[session_.database]["users"];
      // Checked here as well as by the unique index. The index is what makes
      // it true; this is what makes the answer a user_exists instead of a
      // database error nobody can act on.
      if (users.find_one(make_document(kvp("username", account.username)))) {
        return Error{.code = "user_exists", .message = "username is already taken"};
      }
      users.insert_one(account_to_document(account).view());
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not create the account", error.what());
    }
  }

  [[nodiscard]] std::optional<Account> find_by_username(
      const std::string& username) const override {
    return find_one(make_document(kvp("username", username)));
  }

  [[nodiscard]] std::optional<Account> find_by_id(const std::string& user_id) const override {
    return find_one(make_document(kvp("user_id", user_id)));
  }

  [[nodiscard]] std::vector<Account> list() const override {
    std::vector<Account> accounts;
    try {
      auto client = session_.pool.acquire();
      auto cursor = (*client)[session_.database]["users"].find(
          {}, mongocxx::options::find{}.sort(make_document(kvp("created_at", 1))));
      for (const auto& document : cursor) {
        accounts.push_back(account_from(document));
      }
    } catch (const mongocxx::exception& error) {
      DV_LOG_ERROR("Could not list the accounts: {}", error.what());
    }
    return accounts;
  }

  [[nodiscard]] std::optional<Error> update(const Account& account) override {
    try {
      auto client = session_.pool.acquire();
      // $set rather than a whole replacement, and created_at is not in the
      // set: it records when the account came into existence and is not the
      // caller's to rewrite.
      const auto result = (*client)[session_.database]["users"].update_one(
          make_document(kvp("user_id", account.user.id)),
          make_document(kvp(
              "$set", make_document(kvp("username", account.username),
                                    kvp("display_name", account.user.display_name),
                                    kvp("avatar", account.user.avatar),
                                    kvp("role", std::string(models::to_string(account.user.role))),
                                    kvp("salt_hex", account.salt_hex),
                                    kvp("password_hash_hex", account.password_hash_hex)))));
      if (!result || result->matched_count() == 0) {
        return Error{.code = "user_not_found", .message = "no such account"};
      }
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not update the account", error.what());
    }
  }

  [[nodiscard]] std::optional<Error> remove(const std::string& user_id) override {
    try {
      auto client = session_.pool.acquire();
      const auto result =
          (*client)[session_.database]["users"].delete_one(make_document(kvp("user_id", user_id)));
      if (!result || result->deleted_count() == 0) {
        return Error{.code = "user_not_found", .message = "no such account"};
      }
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not delete the account", error.what());
    }
  }

  [[nodiscard]] std::size_t count_with_role(models::Role role) const override {
    try {
      auto client = session_.pool.acquire();
      return static_cast<std::size_t>((*client)[session_.database]["users"].count_documents(
          make_document(kvp("role", std::string(models::to_string(role))))));
    } catch (const mongocxx::exception& error) {
      // Counting is how the last administrator is protected, so a failure here
      // has to read as "there might be only one" rather than as zero. Zero
      // would let the rule pass and the last administrator be deleted.
      DV_LOG_ERROR("Could not count the accounts holding a role: {}", error.what());
      return 1;
    }
  }

 private:
  [[nodiscard]] std::optional<Account> find_one(const bsoncxx::document::value& filter) const {
    try {
      auto client = session_.pool.acquire();
      const auto document = (*client)[session_.database]["users"].find_one(filter.view());
      if (!document) {
        return std::nullopt;
      }
      return account_from(document->view());
    } catch (const mongocxx::exception& error) {
      // Nothing rather than a failure, because the callers are lookups whose
      // "not found" answer is already the safe one: an authentication that
      // cannot read the account refuses the login.
      DV_LOG_ERROR("Could not read an account: {}", error.what());
      return std::nullopt;
    }
  }

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  Session& session_;
};

/// Persistent rooms. See RoomStore.
class MongoRoomStore final : public RoomStore {
 public:
  explicit MongoRoomStore(Session& session) : session_(session) {}

  [[nodiscard]] std::optional<Error> ensure_indexes() {
    try {
      auto client = session_.pool.acquire();
      (*client)[session_.database]["rooms"].create_index(make_document(kvp("id", 1)),
                                                         make_document(kvp("unique", true)));
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not create the room indexes", error.what());
    }
  }

  [[nodiscard]] std::optional<Error> upsert(RoomRecord record) override {
    if (record.created_at == 0) {
      record.created_at = unix_seconds_now();
    }
    try {
      auto client = session_.pool.acquire();
      (*client)[session_.database]["rooms"].update_one(
          make_document(kvp("id", record.id)),
          make_document(
              kvp("$set", make_document(kvp("name", record.name), kvp("owner_id", record.owner_id),
                                        kvp("persistent", record.persistent))),
              // Only on insert, so that rewriting a room does not restate when
              // it was created.
              kvp("$setOnInsert", make_document(kvp("created_at", record.created_at)))),
          mongocxx::options::update{}.upsert(true));
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not write the room", error.what());
    }
  }

  [[nodiscard]] std::optional<RoomRecord> find(const std::string& room_id) const override {
    try {
      auto client = session_.pool.acquire();
      const auto document =
          (*client)[session_.database]["rooms"].find_one(make_document(kvp("id", room_id)));
      if (!document) {
        return std::nullopt;
      }
      return room_from(document->view());
    } catch (const mongocxx::exception& error) {
      DV_LOG_ERROR("Could not read room {}: {}", room_id, error.what());
      return std::nullopt;
    }
  }

  [[nodiscard]] std::vector<RoomRecord> list() const override {
    std::vector<RoomRecord> rooms;
    try {
      auto client = session_.pool.acquire();
      auto cursor = (*client)[session_.database]["rooms"].find(
          {}, mongocxx::options::find{}.sort(make_document(kvp("created_at", 1))));
      for (const auto& document : cursor) {
        rooms.push_back(room_from(document));
      }
    } catch (const mongocxx::exception& error) {
      DV_LOG_ERROR("Could not list the rooms: {}", error.what());
    }
    return rooms;
  }

  [[nodiscard]] std::optional<Error> remove(const std::string& room_id) override {
    try {
      auto client = session_.pool.acquire();
      const auto result =
          (*client)[session_.database]["rooms"].delete_one(make_document(kvp("id", room_id)));
      if (!result || result->deleted_count() == 0) {
        return Error{.code = "room_not_found", .message = "no such room"};
      }
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not delete the room", error.what());
    }
  }

 private:
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  Session& session_;
};

/// The administrative record. See AuditLog.
class MongoAuditLog final : public AuditLog {
 public:
  explicit MongoAuditLog(Session& session) : session_(session) {}

  [[nodiscard]] std::optional<Error> ensure_indexes() {
    try {
      // Not unique: the log is append only and two actions can share a second.
      // Indexed because every read of it is "the newest N".
      auto client = session_.pool.acquire();
      (*client)[session_.database]["audit"].create_index(
          make_document(kvp("timestamp_seconds", -1)));
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not create the audit indexes", error.what());
    }
  }

  [[nodiscard]] std::optional<Error> append(models::AuditEntry entry) override {
    if (entry.timestamp_seconds == 0) {
      entry.timestamp_seconds = unix_seconds_now();
    }
    try {
      auto client = session_.pool.acquire();
      (*client)[session_.database]["audit"].insert_one(
          make_document(kvp("actor_id", entry.actor_id),
                        kvp("actor_username", entry.actor_username), kvp("action", entry.action),
                        kvp("target_id", entry.target_id), kvp("room_id", entry.room_id),
                        kvp("detail", entry.detail),
                        kvp("timestamp_seconds", entry.timestamp_seconds))
              .view());
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not write the audit entry", error.what());
    }
  }

  [[nodiscard]] std::vector<models::AuditEntry> list(int limit,
                                                     const std::string& actor_id) const override {
    std::vector<models::AuditEntry> entries;
    try {
      auto client = session_.pool.acquire();
      auto options = mongocxx::options::find{}
                         .sort(make_document(kvp("timestamp_seconds", -1)))
                         .limit(clamp_audit_limit(limit));
      const auto filter =
          actor_id.empty() ? make_document() : make_document(kvp("actor_id", actor_id));
      auto cursor = (*client)[session_.database]["audit"].find(filter.view(), options);
      for (const auto& document : cursor) {
        entries.push_back(audit_from(document));
      }
    } catch (const mongocxx::exception& error) {
      DV_LOG_ERROR("Could not read the audit log: {}", error.what());
    }
    return entries;
  }

 private:
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  Session& session_;
};

}  // namespace

/// Owns the session and the three stores over it, in that order so that the
/// stores are destroyed before the connection they hold a reference to.
class MongoStores::Impl {
 public:
  Impl(mongocxx::uri uri, std::string database)
      : session_(std::move(uri), std::move(database)),
        users_(session_),
        rooms_(session_),
        audit_(session_) {}

  [[nodiscard]] std::optional<Error> ensure_indexes() {
    if (auto failed = users_.ensure_indexes()) {
      return failed;
    }
    if (auto failed = rooms_.ensure_indexes()) {
      return failed;
    }
    return audit_.ensure_indexes();
  }

  [[nodiscard]] UserStore& users() noexcept { return users_; }
  [[nodiscard]] RoomStore& rooms() noexcept { return rooms_; }
  [[nodiscard]] AuditLog& audit() noexcept { return audit_; }

 private:
  Session session_;
  MongoUserStore users_;
  MongoRoomStore rooms_;
  MongoAuditLog audit_;
};

Result<std::unique_ptr<MongoStores>> MongoStores::open(const std::string& uri,
                                                       const std::string& database,
                                                       int timeout_ms) {
  ensure_driver_instance();

  try {
    mongocxx::uri parsed(with_timeout(uri, timeout_ms));

    auto stores = std::unique_ptr<MongoStores>(
        new MongoStores(std::make_unique<Impl>(std::move(parsed), database)));

    if (auto failed = stores->impl_->ensure_indexes()) {
      return Result<std::unique_ptr<MongoStores>>::failure(*failed);
    }

    DV_LOG_INFO("Connected to MongoDB database '{}' with a {} ms timeout", database, timeout_ms);
    return stores;
  } catch (const mongocxx::exception& error) {
    return Result<std::unique_ptr<MongoStores>>::failure("database_error", error.what());
  }
}

MongoStores::MongoStores(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MongoStores::~MongoStores() = default;

UserStore& MongoStores::users() noexcept {
  return impl_->users();
}

RoomStore& MongoStores::rooms() noexcept {
  return impl_->rooms();
}

AuditLog& MongoStores::audit() noexcept {
  return impl_->audit();
}

}  // namespace dv::server::store
