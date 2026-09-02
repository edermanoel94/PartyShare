#include "store/mongo_store.hpp"

#include <algorithm>
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
#include <mongocxx/pipeline.hpp>
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

/// Reads the restrictions subdocument, which an account written before it
/// existed does not have.
///
/// Absent, of the wrong type, or missing a flag, all answer "nothing is taken
/// away". Tolerant for the reason string_field is, and safe in the same
/// direction the wire parser is: a document nobody understands must not turn
/// into a ban.
models::Restrictions restrictions_from(const bsoncxx::document::view& document) {
  models::Restrictions restrictions;
  const auto element = document["restrictions"];
  if (!element || element.type() != bsoncxx::type::k_document) {
    return restrictions;
  }
  const bsoncxx::document::view nested = element.get_document().value;
  restrictions.banned = bool_field(nested, "banned");
  restrictions.muted = bool_field(nested, "muted");
  restrictions.silenced = bool_field(nested, "silenced");
  restrictions.screen_share_blocked = bool_field(nested, "screen_share_blocked");
  return restrictions;
}

/// Always written whole, all four flags, even when none is set.
///
/// A subdocument rather than four top level fields, so that one $set replaces
/// the lot: writing them separately would let a failure land halfway and leave
/// an account that is banned and not silenced when the administrator asked for
/// both. It is also what makes the terminal tool's write and this one the same
/// operation on the same shape.
bsoncxx::document::value restrictions_to_document(const models::Restrictions& restrictions) {
  return make_document(kvp("banned", restrictions.banned), kvp("muted", restrictions.muted),
                       kvp("silenced", restrictions.silenced),
                       kvp("screen_share_blocked", restrictions.screen_share_blocked));
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
  account.user.restrictions = restrictions_from(document);
  return account;
}

bsoncxx::document::value account_to_document(const Account& account) {
  return make_document(
      kvp("user_id", account.user.id), kvp("username", account.username),
      kvp("display_name", account.user.display_name), kvp("avatar", account.user.avatar),
      kvp("role", std::string(models::to_string(account.user.role))),
      kvp("salt_hex", account.salt_hex), kvp("password_hash_hex", account.password_hash_hex),
      kvp("created_at", account.created_at),
      kvp("restrictions", restrictions_to_document(account.user.restrictions)));
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

models::ChatMessage chat_from(const bsoncxx::document::view& document) {
  models::ChatMessage message;
  const auto id = document["_id"];
  if (id && id.type() == bsoncxx::type::k_oid) {
    message.id = id.get_oid().value.to_string();
  }
  message.room_id = string_field(document, "room_id");
  message.user_id = string_field(document, "user_id");
  message.display_name = string_field(document, "display_name");
  message.text = string_field(document, "text");
  message.timestamp_seconds = int_field(document, "timestamp_seconds");
  return message;
}

models::Notice notice_from(const bsoncxx::document::view& document) {
  models::Notice notice;
  const auto id = document["_id"];
  if (id && id.type() == bsoncxx::type::k_oid) {
    notice.id = id.get_oid().value.to_string();
  }
  notice.user_id = string_field(document, "user_id");
  notice.from_user_id = string_field(document, "from_user_id");
  notice.from_display_name = string_field(document, "from_display_name");
  notice.text = string_field(document, "text");
  notice.created_at = int_field(document, "created_at");
  notice.acknowledged_at = int_field(document, "acknowledged_at");
  return notice;
}

SessionRecord session_from(const bsoncxx::document::view& document) {
  SessionRecord session;
  const auto id = document["_id"];
  if (id && id.type() == bsoncxx::type::k_oid) {
    session.id = id.get_oid().value.to_string();
  }
  session.user_id = string_field(document, "user_id");
  session.ip = string_field(document, "ip");
  session.connected_at = int_field(document, "connected_at");
  session.last_seen_at = int_field(document, "last_seen_at");
  session.ended_at = int_field(document, "ended_at");
  return session;
}

/// An identifier from one of the collections above, back as the object
/// identifier it names.
///
/// Empty when the text is not one. Every identifier this file hands out is the
/// string form of an OID, twenty four hexadecimal characters, so anything else
/// arrived from a client that made it up. Checked here rather than left to the
/// driver, which answers a malformed one by throwing: a query that fails is
/// not the same answer as a row that is not there, and the caller wants the
/// second one.
std::optional<bsoncxx::oid> object_id(const std::string& text) {
  constexpr std::size_t kOidLength = 24;
  const auto is_hex = [](char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
  };
  if (text.size() != kOidLength || !std::ranges::all_of(text, is_hex)) {
    return std::nullopt;
  }
  return bsoncxx::oid{text};
}

/// The refusal a notice identifier that names nothing gets.
///
/// Written out rather than reaching for the memory store's helper, and worded
/// identically to it on purpose: which implementation is behind the server is
/// not something a client can see, so it must not be something a client can
/// tell from an error.
Error no_such_notice() {
  return Error{.code = "notice_not_found", .message = "no such record"};
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
              "$set",
              make_document(
                  kvp("username", account.username), kvp("display_name", account.user.display_name),
                  kvp("avatar", account.user.avatar),
                  kvp("role", std::string(models::to_string(account.user.role))),
                  kvp("salt_hex", account.salt_hex),
                  kvp("password_hash_hex", account.password_hash_hex),
                  kvp("restrictions", restrictions_to_document(account.user.restrictions))))));
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

/// The conversations. See ChatStore.
class MongoChatStore final : public ChatStore {
 public:
  explicit MongoChatStore(Session& session) : session_(session) {}

  [[nodiscard]] std::optional<Error> ensure_indexes() {
    try {
      // Compound, and in this order, because every read is "the newest N of
      // one room": the room narrows the collection and the time orders what is
      // left, which is one index scan rather than a sort over everything that
      // room ever said.
      auto client = session_.pool.acquire();
      (*client)[session_.database]["chat"].create_index(
          make_document(kvp("room_id", 1), kvp("timestamp_seconds", -1), kvp("_id", -1)));
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not create the chat indexes", error.what());
    }
  }

  [[nodiscard]] Result<models::ChatMessage> append(models::ChatMessage message) override {
    if (message.timestamp_seconds == 0) {
      message.timestamp_seconds = unix_seconds_now();
    }
    try {
      auto client = session_.pool.acquire();
      const auto result = (*client)[session_.database]["chat"].insert_one(
          make_document(kvp("room_id", message.room_id), kvp("user_id", message.user_id),
                        kvp("display_name", message.display_name), kvp("text", message.text),
                        kvp("timestamp_seconds", message.timestamp_seconds))
              .view());
      // The identifier the database generated, which is the one `list` will
      // report for this row. Anything else would be an identifier that exists
      // only in the copy the room was shown.
      if (!result) {
        return Result<models::ChatMessage>::failure(
            failure("could not write the message", "the insert was not acknowledged"));
      }
      const auto inserted = result->inserted_id();
      if (inserted.type() != bsoncxx::type::k_oid) {
        return Result<models::ChatMessage>::failure(
            failure("could not write the message", "the insert returned no identifier"));
      }
      message.id = inserted.get_oid().value.to_string();
      return message;
    } catch (const mongocxx::exception& error) {
      return Result<models::ChatMessage>::failure(
          failure("could not write the message", error.what()));
    }
  }

  [[nodiscard]] std::vector<models::ChatMessage> list(const std::string& room_id,
                                                      int limit) const override {
    std::vector<models::ChatMessage> messages;
    try {
      auto client = session_.pool.acquire();
      // Newest first with a limit, which is the only way to ask a database for
      // the end of something without reading the beginning. Reversed below,
      // because oldest first is what the contract hands back.
      //
      // `_id` breaks the tie within one second. Identifiers generated by a
      // single server increase with each document, so two messages sent in the
      // same second still come back in the order they were sent.
      auto options = mongocxx::options::find{}
                         .sort(make_document(kvp("timestamp_seconds", -1), kvp("_id", -1)))
                         .limit(clamp_chat_limit(limit));
      auto cursor = (*client)[session_.database]["chat"].find(
          make_document(kvp("room_id", room_id)).view(), options);
      for (const auto& document : cursor) {
        messages.push_back(chat_from(document));
      }
      std::ranges::reverse(messages);
    } catch (const mongocxx::exception& error) {
      DV_LOG_ERROR("Could not read the conversation of room {}: {}", room_id, error.what());
    }
    return messages;
  }

  [[nodiscard]] std::optional<Error> clear(const std::string& room_id) override {
    try {
      auto client = session_.pool.acquire();
      // delete_many and no check on the count: a room where nobody spoke has
      // nothing to forget, and that is the outcome asked for rather than a
      // failure.
      (*client)[session_.database]["chat"].delete_many(make_document(kvp("room_id", room_id)));
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not clear the conversation", error.what());
    }
  }

 private:
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  Session& session_;
};

/// What administrators told individual accounts. See NoticeStore.
class MongoNoticeStore final : public NoticeStore {
 public:
  explicit MongoNoticeStore(Session& session) : session_(session) {}

  [[nodiscard]] std::optional<Error> ensure_indexes() {
    try {
      // Compound, and in this order, because the only read of this collection
      // is "what does one account still owe an answer to, oldest first". The
      // account narrows it, the flag drops what has been answered, and the
      // time orders what is left, which is one index scan rather than a sort
      // over everything anybody was ever told.
      auto client = session_.pool.acquire();
      (*client)[session_.database]["notices"].create_index(
          make_document(kvp("user_id", 1), kvp("acknowledged_at", 1), kvp("created_at", 1)));
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not create the notice indexes", error.what());
    }
  }

  [[nodiscard]] Result<models::Notice> append(models::Notice notice) override {
    if (notice.created_at == 0) {
      notice.created_at = unix_seconds_now();
    }
    try {
      auto client = session_.pool.acquire();
      const auto result = (*client)[session_.database]["notices"].insert_one(
          make_document(kvp("user_id", notice.user_id), kvp("from_user_id", notice.from_user_id),
                        kvp("from_display_name", notice.from_display_name),
                        kvp("text", notice.text), kvp("created_at", notice.created_at),
                        kvp("acknowledged_at", notice.acknowledged_at))
              .view());
      // The identifier the database generated. It is what the recipient will
      // send back, so a notice delivered under any other one is a notice
      // nobody can acknowledge.
      if (!result) {
        return Result<models::Notice>::failure(
            failure("could not write the notice", "the insert was not acknowledged"));
      }
      const auto inserted = result->inserted_id();
      if (inserted.type() != bsoncxx::type::k_oid) {
        return Result<models::Notice>::failure(
            failure("could not write the notice", "the insert returned no identifier"));
      }
      notice.id = inserted.get_oid().value.to_string();
      return notice;
    } catch (const mongocxx::exception& error) {
      return Result<models::Notice>::failure(failure("could not write the notice", error.what()));
    }
  }

  [[nodiscard]] std::vector<models::Notice> pending_for(const std::string& user_id) const override {
    std::vector<models::Notice> pending;
    try {
      auto client = session_.pool.acquire();
      auto options = mongocxx::options::find{}
                         .sort(make_document(kvp("created_at", 1), kvp("_id", 1)))
                         .limit(NoticeStore::kMaxPendingPerDelivery);
      // Zero and not "absent": every document this file writes carries the
      // field, and one written by a version that did not is one nobody has
      // acknowledged either, so matching zero alone would hide it forever.
      auto cursor = (*client)[session_.database]["notices"].find(
          make_document(
              kvp("user_id", user_id),
              kvp("acknowledged_at",
                  make_document(kvp("$in", bsoncxx::builder::basic::make_array(
                                               std::int64_t{0}, bsoncxx::types::b_null{})))))
              .view(),
          options);
      for (const auto& document : cursor) {
        pending.push_back(notice_from(document));
      }
    } catch (const mongocxx::exception& error) {
      DV_LOG_ERROR("Could not read the notices for {}: {}", user_id, error.what());
    }
    return pending;
  }

  [[nodiscard]] Result<models::Notice> acknowledge(const std::string& notice_id,
                                                   const std::string& user_id) override {
    const auto oid = object_id(notice_id);
    if (!oid.has_value()) {
      return Result<models::Notice>::failure(no_such_notice());
    }
    try {
      auto client = session_.pool.acquire();
      // find_one_and_update rather than an update followed by a read: the
      // answer has to be the row as it now is, and two calls would be two
      // chances for a second session of the same account to change it in
      // between.
      //
      // The filter names the account as well as the notice, which is the whole
      // access check. Somebody else's identifier matches nothing and comes
      // back as `notice_not_found`, exactly as an identifier belonging to
      // nobody does, so a client cannot learn that a notice exists by being
      // refused differently.
      auto options = mongocxx::options::find_one_and_update{}.return_document(
          mongocxx::options::return_document::k_after);
      const auto updated = (*client)[session_.database]["notices"].find_one_and_update(
          make_document(kvp("_id", *oid), kvp("user_id", user_id)).view(),
          // Only when it is still outstanding. The first time somebody said
          // they had read it is the fact worth keeping, so a second
          // acknowledgement leaves the stamp where it was rather than moving
          // it to now.
          make_document(kvp("$set", make_document(kvp("acknowledged_at", unix_seconds_now()))))
              .view(),
          options);
      if (!updated) {
        return Result<models::Notice>::failure(no_such_notice());
      }
      return notice_from(updated->view());
    } catch (const mongocxx::exception& error) {
      return Result<models::Notice>::failure(
          failure("could not acknowledge the notice", error.what()));
    }
  }

  [[nodiscard]] std::optional<Error> clear_for(const std::string& user_id) override {
    try {
      auto client = session_.pool.acquire();
      // delete_many and no check on the count, for ChatStore::clear's reason:
      // an account nobody ever wrote to has nothing to forget, and that is the
      // outcome asked for rather than a failure.
      (*client)[session_.database]["notices"].delete_many(make_document(kvp("user_id", user_id)));
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not clear the notices", error.what());
    }
  }

 private:
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  Session& session_;
};

/// Who is connected, and from where. See SessionStore.
class MongoSessionStore final : public SessionStore {
 public:
  explicit MongoSessionStore(Session& session) : session_(session) {}

  [[nodiscard]] std::optional<Error> ensure_indexes() {
    try {
      auto client = session_.pool.acquire();
      // Two, because the two readers of this collection ask different
      // questions. tools/dbadmin asks "who is here", which is every open row
      // newest first; somebody looking at one account asks for its history,
      // which is that account's rows in time order.
      auto sessions = (*client)[session_.database]["sessions"];
      sessions.create_index(make_document(kvp("ended_at", 1), kvp("last_seen_at", -1)));
      sessions.create_index(make_document(kvp("user_id", 1), kvp("connected_at", -1)));
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not create the session indexes", error.what());
    }
  }

  [[nodiscard]] Result<SessionRecord> open(SessionRecord record) override {
    const std::int64_t now = unix_seconds_now();
    if (record.connected_at == 0) {
      record.connected_at = now;
    }
    if (record.last_seen_at == 0) {
      record.last_seen_at = record.connected_at;
    }
    try {
      auto client = session_.pool.acquire();
      const auto result = (*client)[session_.database]["sessions"].insert_one(
          make_document(kvp("user_id", record.user_id), kvp("ip", record.ip),
                        kvp("connected_at", record.connected_at),
                        kvp("last_seen_at", record.last_seen_at), kvp("ended_at", record.ended_at))
              .view());
      if (!result) {
        return Result<SessionRecord>::failure(
            failure("could not record the session", "the insert was not acknowledged"));
      }
      const auto inserted = result->inserted_id();
      if (inserted.type() != bsoncxx::type::k_oid) {
        return Result<SessionRecord>::failure(
            failure("could not record the session", "the insert returned no identifier"));
      }
      record.id = inserted.get_oid().value.to_string();
      return record;
    } catch (const mongocxx::exception& error) {
      return Result<SessionRecord>::failure(failure("could not record the session", error.what()));
    }
  }

  [[nodiscard]] std::optional<Error> touch(const std::vector<std::string>& ids) override {
    if (ids.empty()) {
      return std::nullopt;
    }
    try {
      bsoncxx::builder::basic::array wanted;
      for (const std::string& id : ids) {
        if (const auto oid = object_id(id)) {
          wanted.append(*oid);
        }
      }

      auto client = session_.pool.acquire();
      // One update for the whole set. Called once per heartbeat with the
      // server's own lock held, so the difference between this and a write per
      // session is the difference between one round trip every five seconds
      // and one per connected person.
      //
      // Restricted to rows that are still open, so a session closed a
      // millisecond ago by the socket dropping is not reopened in spirit by a
      // heartbeat that had already collected its identifier.
      (*client)[session_.database]["sessions"].update_many(
          make_document(kvp("_id", make_document(kvp("$in", wanted))), kvp("ended_at", 0)).view(),
          make_document(kvp("$set", make_document(kvp("last_seen_at", unix_seconds_now()))))
              .view());
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not refresh the sessions", error.what());
    }
  }

  [[nodiscard]] std::optional<Error> close(const std::string& id) override {
    const auto oid = object_id(id);
    if (!oid.has_value()) {
      return std::nullopt;
    }
    try {
      auto client = session_.pool.acquire();
      // `ended_at: 0` in the filter is what makes closing twice harmless: the
      // second call matches nothing and the first one's timestamp, which is
      // the one that knew when the person actually left, stays.
      (*client)[session_.database]["sessions"].update_one(
          make_document(kvp("_id", *oid), kvp("ended_at", 0)).view(),
          make_document(kvp("$set", make_document(kvp("ended_at", unix_seconds_now())))).view());
      return std::nullopt;
    } catch (const mongocxx::exception& error) {
      return failure("could not close the session", error.what());
    }
  }

  [[nodiscard]] std::size_t close_open() override {
    try {
      auto client = session_.pool.acquire();
      // An aggregation pipeline as the update, so that each row is stamped
      // with its own `last_seen_at` rather than all of them with the moment of
      // recovery. A server that was killed on Friday and started on Monday did
      // not have anybody connected over the weekend, and a plain $set of "now"
      // is exactly that claim.
      const auto result = (*client)[session_.database]["sessions"].update_many(
          make_document(kvp("ended_at", 0)).view(),
          mongocxx::pipeline{}.add_fields(make_document(kvp("ended_at", "$last_seen_at"))));
      return result ? static_cast<std::size_t>(result->modified_count()) : 0;
    } catch (const mongocxx::exception& error) {
      DV_LOG_ERROR("Could not close the sessions left open by a previous run: {}", error.what());
      return 0;
    }
  }

  [[nodiscard]] std::vector<SessionRecord> list_open() const override {
    std::vector<SessionRecord> open;
    try {
      auto client = session_.pool.acquire();
      auto options = mongocxx::options::find{}.sort(make_document(kvp("last_seen_at", -1)));
      auto cursor = (*client)[session_.database]["sessions"].find(
          make_document(kvp("ended_at", 0)).view(), options);
      for (const auto& document : cursor) {
        open.push_back(session_from(document));
      }
    } catch (const mongocxx::exception& error) {
      DV_LOG_ERROR("Could not read the open sessions: {}", error.what());
    }
    return open;
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

/// Owns the session and the stores over it, in that order so that the stores
/// are destroyed before the connection they hold a reference to.
class MongoStores::Impl {
 public:
  Impl(mongocxx::uri uri, std::string database)
      : session_(std::move(uri), std::move(database)),
        users_(session_),
        rooms_(session_),
        chat_(session_),
        notices_(session_),
        sessions_(session_),
        audit_(session_) {}

  [[nodiscard]] std::optional<Error> ensure_indexes() {
    if (auto failed = users_.ensure_indexes()) {
      return failed;
    }
    if (auto failed = rooms_.ensure_indexes()) {
      return failed;
    }
    if (auto failed = chat_.ensure_indexes()) {
      return failed;
    }
    if (auto failed = notices_.ensure_indexes()) {
      return failed;
    }
    if (auto failed = sessions_.ensure_indexes()) {
      return failed;
    }
    return audit_.ensure_indexes();
  }

  [[nodiscard]] UserStore& users() noexcept { return users_; }
  [[nodiscard]] RoomStore& rooms() noexcept { return rooms_; }
  [[nodiscard]] ChatStore& chat() noexcept { return chat_; }
  [[nodiscard]] NoticeStore& notices() noexcept { return notices_; }
  [[nodiscard]] SessionStore& sessions() noexcept { return sessions_; }
  [[nodiscard]] AuditLog& audit() noexcept { return audit_; }

 private:
  Session session_;
  MongoUserStore users_;
  MongoRoomStore rooms_;
  MongoChatStore chat_;
  MongoNoticeStore notices_;
  MongoSessionStore sessions_;
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

ChatStore& MongoStores::chat() noexcept {
  return impl_->chat();
}

NoticeStore& MongoStores::notices() noexcept {
  return impl_->notices();
}

SessionStore& MongoStores::sessions() noexcept {
  return impl_->sessions();
}

AuditLog& MongoStores::audit() noexcept {
  return impl_->audit();
}

}  // namespace dv::server::store
