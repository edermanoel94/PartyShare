#pragma once

#include <memory>
#include <string>

#include <dv/core/result.hpp>

#include "store/audit_log.hpp"
#include "store/chat_store.hpp"
#include "store/notice_store.hpp"
#include "store/room_store.hpp"
#include "store/session_store.hpp"
#include "store/user_store.hpp"

namespace dv::server::store {

/// The stores backed by one MongoDB database.
///
/// A single object rather than one per store, because they share a connection
/// pool and a lifetime: the driver has one global instance per process, and
/// handing out independent stores would mean several places that have to agree
/// about when it is created and destroyed.
///
/// Collections, all in the database named in the configuration:
///
///   users     one document per account, unique index on `username`
///   rooms     one document per persistent room, unique index on `id`
///   chat      one document per message, index on `room_id` and time
///   notices   one document per administrator's message to one account
///   sessions  one document per connected session, open and closed alike
///   audit     one document per administrative action, index on `timestamp`
///
/// Every call is synchronous and happens with the server's lock held. That is
/// affordable because none of them is on the path of a media packet: an
/// account is read once per connection, and an administrative action is rare.
/// It is also why the connection is opened with a short server selection
/// timeout, in `open` below: the default of thirty seconds would turn an
/// unreachable database into a server that answers nobody for half a minute.
class MongoStores {
 public:
  /// Connects and ensures the indexes exist.
  ///
  /// Fails rather than degrading to memory. A server told to use a database
  /// and started without one would be a server whose accounts silently do not
  /// persist, and the first sign of it would be an administrator that vanished
  /// overnight.
  [[nodiscard]] static Result<std::unique_ptr<MongoStores>> open(const std::string& uri,
                                                                 const std::string& database,
                                                                 int timeout_ms);

  ~MongoStores();

  MongoStores(const MongoStores&) = delete;
  MongoStores& operator=(const MongoStores&) = delete;
  MongoStores(MongoStores&&) = delete;
  MongoStores& operator=(MongoStores&&) = delete;

  [[nodiscard]] UserStore& users() noexcept;
  [[nodiscard]] RoomStore& rooms() noexcept;
  [[nodiscard]] ChatStore& chat() noexcept;
  [[nodiscard]] NoticeStore& notices() noexcept;
  [[nodiscard]] SessionStore& sessions() noexcept;
  [[nodiscard]] AuditLog& audit() noexcept;

 private:
  class Impl;

  explicit MongoStores(std::unique_ptr<Impl> impl);

  /// The driver's headers are not in this one, so that everything including
  /// hub.cpp can include it without dragging bsoncxx into a translation unit
  /// that has no business knowing about it.
  std::unique_ptr<Impl> impl_;
};

}  // namespace dv::server::store
