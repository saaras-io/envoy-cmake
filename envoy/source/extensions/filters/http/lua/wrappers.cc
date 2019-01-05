#include "extensions/filters/http/lua/wrappers.h"

#include "common/http/utility.h"

#include "extensions/filters/common/lua/wrappers.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Lua {

HeaderMapIterator::HeaderMapIterator(HeaderMapWrapper& parent) : parent_(parent) {
  entries_.reserve(parent_.headers_.size());
  parent_.headers_.iterate(
      [](const Http::HeaderEntry& header, void* context) -> Http::HeaderMap::Iterate {
        HeaderMapIterator* iterator = static_cast<HeaderMapIterator*>(context);
        iterator->entries_.push_back(&header);
        return Http::HeaderMap::Iterate::Continue;
      },
      this);
}

int HeaderMapIterator::luaPairsIterator(lua_State* state) {
  if (current_ == entries_.size()) {
    parent_.iterator_.reset();
    return 0;
  } else {
    lua_pushstring(state, entries_[current_]->key().c_str());
    lua_pushstring(state, entries_[current_]->value().c_str());
    current_++;
    return 2;
  }
}

int HeaderMapWrapper::luaAdd(lua_State* state) {
  checkModifiable(state);

  const char* key = luaL_checkstring(state, 2);
  const char* value = luaL_checkstring(state, 3);
  headers_.addCopy(Http::LowerCaseString(key), value);
  return 0;
}

int HeaderMapWrapper::luaGet(lua_State* state) {
  const char* key = luaL_checkstring(state, 2);
  const Http::HeaderEntry* entry = headers_.get(Http::LowerCaseString(key));
  if (entry != nullptr) {
    lua_pushstring(state, entry->value().c_str());
    return 1;
  } else {
    return 0;
  }
}

int HeaderMapWrapper::luaPairs(lua_State* state) {
  if (iterator_.get() != nullptr) {
    luaL_error(state, "cannot create a second iterator before completing the first");
  }

  // The way iteration works is we create an iteration wrapper that snaps pointers to all of
  // the headers. We don't allow modification while an iterator is active. This means that
  // currently if a script breaks out of iteration, further modifications will not be possible
  // because we don't know if they may resume iteration in the future and it isn't safe. There
  // are potentially better ways of handling this but due to GC of the iterator it's very
  // difficult to control safety without tracking every allocated iterator and invalidating them
  // if the map is modified.
  iterator_.reset(HeaderMapIterator::create(state, *this), true);
  lua_pushcclosure(state, HeaderMapIterator::static_luaPairsIterator, 1);
  return 1;
}

int HeaderMapWrapper::luaReplace(lua_State* state) {
  checkModifiable(state);

  const char* key = luaL_checkstring(state, 2);
  const char* value = luaL_checkstring(state, 3);
  const Http::LowerCaseString lower_key(key);

  Http::HeaderEntry* entry = headers_.get(lower_key);
  if (entry != nullptr) {
    entry->value(value, strlen(value));
  } else {
    headers_.addCopy(lower_key, value);
  }

  return 0;
}

int HeaderMapWrapper::luaRemove(lua_State* state) {
  checkModifiable(state);

  const char* key = luaL_checkstring(state, 2);
  headers_.remove(Http::LowerCaseString(key));
  return 0;
}

void HeaderMapWrapper::checkModifiable(lua_State* state) {
  if (iterator_.get() != nullptr) {
    luaL_error(state, "header map cannot be modified while iterating");
  }

  if (!cb_()) {
    luaL_error(state, "header map can no longer be modified");
  }
}

int RequestInfoWrapper::luaProtocol(lua_State* state) {
  lua_pushstring(state, Http::Utility::getProtocolString(request_info_.protocol().value()).c_str());
  return 1;
}

int RequestInfoWrapper::luaDynamicMetadata(lua_State* state) {
  if (dynamic_metadata_wrapper_.get() != nullptr) {
    dynamic_metadata_wrapper_.pushStack();
  } else {
    dynamic_metadata_wrapper_.reset(DynamicMetadataMapWrapper::create(state, *this), true);
  }
  return 1;
}

DynamicMetadataMapIterator::DynamicMetadataMapIterator(DynamicMetadataMapWrapper& parent)
    : parent_{parent}, current_{parent_.requestInfo().dynamicMetadata().filter_metadata().begin()} {
}

RequestInfo::RequestInfo& DynamicMetadataMapWrapper::requestInfo() { return parent_.request_info_; }

int DynamicMetadataMapIterator::luaPairsIterator(lua_State* state) {
  if (current_ == parent_.requestInfo().dynamicMetadata().filter_metadata().end()) {
    parent_.iterator_.reset();
    return 0;
  }

  lua_pushstring(state, current_->first.c_str());
  Filters::Common::Lua::MetadataMapHelper::createTable(state, current_->second.fields());

  current_++;
  return 2;
}

int DynamicMetadataMapWrapper::luaGet(lua_State* state) {
  const char* filter_name = luaL_checkstring(state, 2);
  const auto& metadata = requestInfo().dynamicMetadata().filter_metadata();
  const auto filter_it = metadata.find(filter_name);
  if (filter_it == metadata.end()) {
    return 0;
  }

  Filters::Common::Lua::MetadataMapHelper::createTable(state, filter_it->second.fields());
  return 1;
}

int DynamicMetadataMapWrapper::luaSet(lua_State* state) {
  if (iterator_.get() != nullptr) {
    luaL_error(state, "dynamic metadata map cannot be modified while iterating");
  }

  // TODO(dio): Allow to set dynamic metadata using a table.
  const char* filter_name = luaL_checkstring(state, 2);
  const char* key = luaL_checkstring(state, 3);
  const char* value = luaL_checkstring(state, 4);
  requestInfo().setDynamicMetadata(filter_name, MessageUtil::keyValueStruct(key, value));
  return 0;
}

int DynamicMetadataMapWrapper::luaPairs(lua_State* state) {
  if (iterator_.get() != nullptr) {
    luaL_error(state, "cannot create a second iterator before completing the first");
  }

  iterator_.reset(DynamicMetadataMapIterator::create(state, *this), true);
  lua_pushcclosure(state, DynamicMetadataMapIterator::static_luaPairsIterator, 1);
  return 1;
}

} // namespace Lua
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
