#pragma once
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <initializer_list>
#include <memory>
#include <mysql++/mysql++.h>
#include "log.h"
/*
Utilization examples:
With list of arguments:
```
std::initializer_list<DBArgOption> args = {
  dbReconnectOption,
  dbConnectTimeoutOption(5),
  dbReadTimeoutOption(5),
  dbWriteTimeoutOption(5),
  dbFoundRowsOption
};
apply_mysql_options(conn1, args);
apply_mysql_options(conn2, args);
apply_mysql_options(conn3, args);
```
As a variadic function:
```
apply_mysql_options(conn, dbConnectTimeoutOption(5), dbReadTimeoutOption(5));
```
Variadic function, only setting reconnectionOption:
```
apply_mysql_options(conn, dbReconnectOption);
```
Variadic function, no time set(default: 5) and out of order:
```
apply_mysql_options(conn, dbFoundRowsOption, dbConnectTimeoutOption);
```
Note that ```apply_mysql_options(conn)``` should do nothing.
*/
enum class DbOptionId { Reconnect, ConnectTimeout, ReadTimeout, WriteTimeout, FoundRows };
using Value = std::variant<int, bool, std::string>;
using Kv    = std::pair<DbOptionId, Value>;
struct Key
{
	DbOptionId id;
	template <class T>
	auto operator()(T&& v) const {
		return Kv{ id, Value{std::forward<T>(v)} };
	}
};
inline constexpr int defaultTimeoutSec = 5;
inline constexpr Key dbReconnectOption{DbOptionId::Reconnect};
inline constexpr Key dbConnectTimeoutOption{DbOptionId::ConnectTimeout};
inline constexpr Key dbReadTimeoutOption{DbOptionId::ReadTimeout};
inline constexpr Key dbWriteTimeoutOption{DbOptionId::WriteTimeout};
inline constexpr Key dbFoundRowsOption{DbOptionId::FoundRows};
using DBArgOption   = std::variant<Key, Kv>;
template<class T>
concept ArgLike = std::is_same_v<std::decay_t<T>, Key> || std::is_same_v<std::decay_t<T>, Kv>;
template<typename Opt, typename... Args>
inline void set_opt(mysqlpp::Connection& conn, Args&&... args) {
    auto p = std::make_unique<Opt>(std::forward<Args>(args)...);
    conn.set_option(p.release());
}
// Flags (no value). Example: passing `dbReconnectOption` alone sets reconnect=true.
inline void process_arg(mysqlpp::Connection& conn, const Key& k) {
    DbOptionId id = k.id;
    switch(id) {
    case DbOptionId::Reconnect:
        set_opt<mysqlpp::ReconnectOption>(conn, true);
        break;
    case DbOptionId::FoundRows:
        set_opt<mysqlpp::FoundRowsOption>(conn, true);
        break;
    case DbOptionId::ConnectTimeout:
        set_opt<mysqlpp::ConnectTimeoutOption>(conn, defaultTimeoutSec);
        break;
    case DbOptionId::ReadTimeout:
        set_opt<mysqlpp::ReadTimeoutOption>(conn, defaultTimeoutSec);
        break;
    case DbOptionId::WriteTimeout:
        set_opt<mysqlpp::WriteTimeoutOption>(conn, defaultTimeoutSec);
        break;
    default:
        ERROR("Unknown DB id(%d) Option without value.\n", static_cast<int>(id));
        break;
    }
}
template<class T, class MakeUnique>
inline void set_as(mysqlpp::Connection& conn, const Value& val, MakeUnique make_unique) {
    if (const T* p = std::get_if<T>(&val)) {
        conn.set_option(make_unique(*p).release());
    }
}
// Key/value pairs. Example: dbConnectTimeoutOption(5) sets a 5s connect timeout.
inline void process_arg(mysqlpp::Connection& conn, const Kv& kv) {
    const auto& [id, val] = kv;
    switch(id) {
    case DbOptionId::ConnectTimeout:
        set_as<int>(conn, val, [&](int x) { return std::make_unique<mysqlpp::ConnectTimeoutOption>(x); });
        break;
    case DbOptionId::ReadTimeout:
        set_as<int>(conn, val, [&](int x) { return std::make_unique<mysqlpp::ReadTimeoutOption>(x); });
        break;
    case DbOptionId::WriteTimeout:
        set_as<int>(conn, val, [&](int x) { return std::make_unique<mysqlpp::WriteTimeoutOption>(x); });
        break;
    case DbOptionId::Reconnect:
        set_as<bool>(conn, val, [&](bool x) { return std::make_unique<mysqlpp::ReconnectOption>(x); });
        break;
    case DbOptionId::FoundRows:
        set_as<bool>(conn, val, [&](bool x) { return std::make_unique<mysqlpp::FoundRowsOption>(x); });
        break;
    default:
        std::string valueStr;
        if (auto p = std::get_if<std::string>(&val)) {
            valueStr = *p;
        } else if (auto p = std::get_if<int>(&val)) {
            valueStr = std::to_string(*p);
        } else if (auto p = std::get_if<bool>(&val)) {
            valueStr = *p ? "true" : "false";
        } else {
            valueStr = "<unknown type>";
        }
        ERROR("Unknown DB id(%d) Option with value (%s).\n", static_cast<int>(id), valueStr.c_str());
        break;
    }
}
template<ArgLike... Args>
inline void apply_mysql_options(mysqlpp::Connection& conn, Args&&... args) {
    (process_arg(conn, std::forward<Args>(args)), ...);
}
inline void apply_mysql_options(mysqlpp::Connection& conn,
                                std::initializer_list<DBArgOption> args) {
    for (const auto& a : args) {
        std::visit([&](const auto& x) { process_arg(conn, x); }, a);
    }
}