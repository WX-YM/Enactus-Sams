#include "anvil/db/migration_step.h"

#include <utility>

#include <mongocxx/client.hpp>

namespace anvil::db {

StepContext::StepContext(mongocxx::client& client, std::string_view database,
                         std::string_view collection) noexcept
    : writes_{}, client_{&client}, database_{database}, collection_{collection} {}

void StepContext::set(bsoncxx::types::bson_value::value id, bsoncxx::document::value fields) {
    writes_.push_back(StepWrite{std::move(id), std::move(fields), StepWriteKind::Set});
}

void StepContext::unset(bsoncxx::types::bson_value::value id, bsoncxx::document::value fields) {
    writes_.push_back(StepWrite{std::move(id), std::move(fields), StepWriteKind::Unset});
}

}  // namespace anvil::db
