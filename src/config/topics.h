#pragma once
#include <array>
#include "anvil/notifications/topics.h"

inline constexpr std::array<anvil::notifications::TopicSpec, 1> kTopics{{
    {"candidate_referred", anvil::notifications::FanOut::Unicast, 7}, // 7 days retention
}};

inline constexpr std::array<anvil::notifications::TemplateSpec, 1> kTemplates{{
    {"candidate_referred", "en", "Candidate Referred", "A candidate has been referred to your team. Please schedule an interview."},
}};
