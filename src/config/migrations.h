#pragma once
#include <array>
#include "anvil/db/migrations.h"

inline constexpr std::array<anvil::db::MigrationStep, 0> kSteps{{}};
inline constexpr std::array<anvil::db::CollectionOptions, 0> kCollectionOptions{{}};
inline constexpr uint32_t kSchemaVersion = 1;
