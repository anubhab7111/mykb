#include "core/signals.hpp"

#include <cstddef>
#include <gtest/gtest.h>
#include <optional>
#include <set>
#include <variant>
#include <vector>

using namespace omenkbd;

TEST(Signals, UrgencyNamesAreCaseInsensitive) {
    EXPECT_EQ(urgency_from_name("Critical"), Urgency::Critical);
    EXPECT_EQ(urgency_from_name("LOW"), Urgency::Low);
    EXPECT_FALSE(urgency_from_name("urgent").has_value());
}

TEST(Signals, UrgencyOrdersLowBelowNormalBelowCritical) {
    EXPECT_LT(Urgency::Low, Urgency::Normal);
    EXPECT_LT(Urgency::Normal, Urgency::Critical);
}

TEST(Signals, OnlyStartingSignalsMayStartAReaction) {
    EXPECT_TRUE(may_start_reaction(signal::WindowOpened{}));
    EXPECT_TRUE(may_start_reaction(signal::AgentWaiting{"s", std::nullopt}));
    EXPECT_FALSE(may_start_reaction(signal::FocusChanged{"a", "b"}));
    EXPECT_FALSE(may_start_reaction(signal::AgentWorking{"s"}));
    EXPECT_FALSE(may_start_reaction(signal::UrgentReset{}));
}

TEST(Signals, UrgencyNamesAreLowercaseAndRoundTrip) {
    EXPECT_EQ(name(Urgency::Low), "low");
    EXPECT_EQ(name(Urgency::Normal), "normal");
    EXPECT_EQ(name(Urgency::Critical), "critical");
    for (Urgency u : {Urgency::Low, Urgency::Normal, Urgency::Critical}) {
        EXPECT_EQ(urgency_from_name(name(u)), u);
    }
    EXPECT_EQ(urgency_from_name("nOrMaL"), Urgency::Normal);
    EXPECT_FALSE(urgency_from_name("").has_value());
}

TEST(Signals, EverySignalIsClassifiedAsTheSpecSays) {
    const std::vector<Signal> starting{
        signal::WorkspaceSwitched{1, 2},
        signal::WindowOpened{},
        signal::WindowClosed{},
        signal::UrgentRaised{"a"},
        signal::Notified{"app", "entry", Urgency::Normal},
        signal::AgentWaiting{"s", std::nullopt},
        signal::AgentDone{"s", "w"},
    };
    const std::vector<Signal> others{
        signal::UrgentCleared{"a"}, signal::UrgentReset{},   signal::FocusChanged{"a", "b"},
        signal::AgentWorking{"s"},  signal::AgentEnded{"s"},
    };
    std::set<std::size_t> alternatives;
    for (const Signal &s : starting) {
        EXPECT_TRUE(may_start_reaction(s)) << "alternative " << s.index();
        alternatives.insert(s.index());
    }
    for (const Signal &s : others) {
        EXPECT_FALSE(may_start_reaction(s)) << "alternative " << s.index();
        alternatives.insert(s.index());
    }
    // Each alternative sits in exactly one list, so adding one to Signal fails here until it is
    // classified.
    EXPECT_EQ(starting.size() + others.size(), std::variant_size_v<Signal>);
    EXPECT_EQ(alternatives.size(), std::variant_size_v<Signal>);
}

TEST(Signals, EqualityIsByAlternativeAndByEveryField) {
    const std::vector<Signal> distinct{
        signal::WorkspaceSwitched{1, 2},
        signal::WorkspaceSwitched{1, 3},
        signal::WorkspaceSwitched{2, 2},
        signal::WindowOpened{},
        signal::WindowClosed{},
        signal::UrgentRaised{"a"},
        signal::UrgentRaised{"b"},
        signal::UrgentCleared{"a"},
        signal::UrgentCleared{"b"},
        signal::UrgentReset{},
        signal::FocusChanged{"a", "x"},
        signal::FocusChanged{"b", "x"},
        signal::FocusChanged{"a", "y"},
        signal::Notified{"app", "entry", Urgency::Normal},
        signal::Notified{"other", "entry", Urgency::Normal},
        signal::Notified{"app", "other", Urgency::Normal},
        signal::Notified{"app", "entry", Urgency::Critical},
        signal::AgentWaiting{"s", std::nullopt},
        signal::AgentWaiting{"s", "w"},
        signal::AgentWaiting{"t", std::nullopt},
        signal::AgentWorking{"s"},
        signal::AgentWorking{"t"},
        signal::AgentDone{"s", std::nullopt},
        signal::AgentDone{"s", "w"},
        signal::AgentDone{"t", std::nullopt},
        signal::AgentEnded{"s"},
        signal::AgentEnded{"t"},
    };
    // Every signal equals itself and no other one in the list.
    for (std::size_t i = 0; i < distinct.size(); ++i) {
        for (std::size_t j = 0; j < distinct.size(); ++j) {
            EXPECT_EQ(distinct[i] == distinct[j], i == j) << i << " vs " << j;
        }
    }
}
