// Hyprland socket2 lines (pure parser) and the tracker that turns parsed events into signals.

#include "sources/hyprland.hpp"
#include "sources/hyprland_tracker.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace omenkbd;

namespace {

using Out = std::vector<TrackerOut>;

template <class S> TrackerOut sig(S s) { return TrackerOut{Signal{std::move(s)}}; }

TrackerOut check() { return TrackerOut{CheckFullscreen{}}; }

TrackerOut submap_cleared() { return TrackerOut{event::Submap{std::nullopt}}; }

TrackerOut fullscreen_cleared() {
    return TrackerOut{FactUpdate{fact::FullscreenApp{std::nullopt}}};
}

/// What Hyprland sends when `address` (class `app_class`) gets the focus.
void focus(HyprlandTracker &t, const std::string &app_class, const std::string &address) {
    (void)t.on(hypr::ActiveWindow{app_class});
    (void)t.on(hypr::ActiveWindowV2{address});
}

} // namespace

TEST(HyprlandParse, ParsesEveryEventWeUse) {
    EXPECT_EQ(parse_socket2_line("submap>>resize"), HyprEvent{hypr::Submap{"resize"}});
    EXPECT_EQ(parse_socket2_line("workspacev2>>2,2"), HyprEvent{hypr::WorkspaceV2{2}});
    EXPECT_EQ(parse_socket2_line("workspacev2>>-99,special:magic"),
              HyprEvent{hypr::WorkspaceV2{-99}})
        << "special workspaces have negative ids; the tracker ignores them";
    EXPECT_EQ(parse_socket2_line("workspacev2>>3,a, b"), HyprEvent{hypr::WorkspaceV2{3}});
    EXPECT_EQ(parse_socket2_line("workspacev2>>7"), HyprEvent{hypr::WorkspaceV2{7}});
    EXPECT_EQ(parse_socket2_line("openwindow>>55666f363b20,2,Alacritty,~/dev"),
              HyprEvent{hypr::OpenWindow{"55666f363b20"}});
    EXPECT_EQ(parse_socket2_line("closewindow>>55666f363b20"),
              HyprEvent{hypr::CloseWindow{"55666f363b20"}});
    EXPECT_EQ(parse_socket2_line("urgent>>55666f363b20"), HyprEvent{hypr::Urgent{"55666f363b20"}});
    EXPECT_EQ(parse_socket2_line("activewindow>>Alacritty,~/dev"),
              HyprEvent{hypr::ActiveWindow{"Alacritty"}});
    EXPECT_EQ(parse_socket2_line("activewindow>>,"), HyprEvent{hypr::ActiveWindow{""}})
        << "nothing focused";
    EXPECT_EQ(parse_socket2_line("activewindowv2>>55666f363b20"),
              HyprEvent{hypr::ActiveWindowV2{"55666f363b20"}});
    EXPECT_EQ(parse_socket2_line("activewindowv2>>"), HyprEvent{hypr::ActiveWindowV2{""}})
        << "empty = nothing focused";
    EXPECT_EQ(parse_socket2_line("fullscreen>>1"), HyprEvent{hypr::Fullscreen{true}});
    EXPECT_EQ(parse_socket2_line("fullscreen>>0"), HyprEvent{hypr::Fullscreen{false}});
}

TEST(HyprlandParse, TitlesWithCommasAndDoubleArrowsNeverShiftFields) {
    EXPECT_EQ(parse_socket2_line("openwindow>>abc,1,Alacritty,a, b >> c,d"),
              HyprEvent{hypr::OpenWindow{"abc"}});
    EXPECT_EQ(parse_socket2_line("activewindow>>firefox,Title, with >> and ,commas"),
              HyprEvent{hypr::ActiveWindow{"firefox"}});
    EXPECT_EQ(parse_socket2_line("workspacev2>>4,>>,5,x"), HyprEvent{hypr::WorkspaceV2{4}});
    EXPECT_EQ(parse_socket2_line("activewindow>>,>>,>>"), HyprEvent{hypr::ActiveWindow{""}})
        << "the class is the text before the first comma, even when it is empty";
}

// The events carry no title field at all, so a title cannot be kept. Nothing about the title
// events is parsed either.
TEST(HyprlandParse, TitleEventsAreNotParsed) {
    EXPECT_EQ(parse_socket2_line("windowtitle>>55666f363b20"), std::nullopt);
    EXPECT_EQ(parse_socket2_line("windowtitlev2>>55666f363b20,Some title, with commas"),
              std::nullopt);
    EXPECT_EQ(parse_socket2_line("windowtitlev2>>"), std::nullopt);
}

TEST(HyprlandParse, MalformedLinesAreIgnoredNotThrown) {
    constexpr std::size_t kHundredKb = std::size_t{100} * 1024;
    const std::string big(kHundredKb, 'x');
    const std::string big_hex(kHundredKb, 'a');
    const std::string big_digits(kHundredKb, '9');
    const std::vector<std::string> bad{
        "workspacev2>>abc,x",
        "workspacev2>>",
        "workspacev2>>,x",
        "workspacev2>>1x,y",
        "workspacev2>> 1,y",
        "workspacev2>>+1,y",
        "workspacev2>>99999999999999999999,x",
        "fullscreen>>2",
        "fullscreen>>",
        "fullscreen>>01",
        "activewindowv2",
        "activewindow",
        ">>",
        "",
        "an unknown event>>x",
        "unknown>>abc",
        "submapx>>resize",
        "openwindow>>,1,Alacritty,t",
        "openwindow>>0x,1,Alacritty,t",
        "closewindow>>",
        "urgent>>",
        // 100 KB: whatever the line holds, it is dropped whole.
        big,
        "unknown>>" + big,
        "workspacev2>>" + big_digits,
        "workspacev2>>1," + big,
        "openwindow>>abc,1,c," + big,
        "activewindow>>firefox," + big,
        "activewindow>>" + big,
        "activewindowv2>>" + big_hex,
        "closewindow>>" + big_hex,
        "submap>>" + big,
    };
    for (const std::string &line : bad) {
        EXPECT_EQ(parse_socket2_line(line), std::nullopt)
            << "a line of " << line.size() << " bytes";
    }
}

TEST(HyprlandParse, AddressesAreNormalized) {
    EXPECT_EQ(parse_socket2_line("activewindowv2>>0xABCdef"),
              HyprEvent{hypr::ActiveWindowV2{"abcdef"}});
    EXPECT_EQ(parse_socket2_line("activewindowv2>>55666F363B20"),
              HyprEvent{hypr::ActiveWindowV2{"55666f363b20"}});
    EXPECT_EQ(parse_socket2_line("openwindow>>0XAB12,1,c,t"), HyprEvent{hypr::OpenWindow{"ab12"}});
    EXPECT_EQ(parse_socket2_line("closewindow>>0xAB12"), HyprEvent{hypr::CloseWindow{"ab12"}});
    EXPECT_EQ(parse_socket2_line("urgent>>0xAB12"), HyprEvent{hypr::Urgent{"ab12"}});
}

// T14-5: a field that is not an address makes the line malformed. Only a genuinely empty
// `activewindowv2` means "nothing focused"; an invalid one must not pass for it.
TEST(HyprlandParse, AnInvalidAddressMakesTheLineMalformed) {
    const std::string hex64(64, 'a');
    const std::string hex65(65, 'a');
    EXPECT_EQ(parse_socket2_line("urgent>>" + hex64), HyprEvent{hypr::Urgent{hex64}})
        << "64 digits is the longest address";
    for (const std::string &bad : {std::string("zz"), std::string("a1-b"), std::string("0x"),
                                   std::string("0x0xab"), std::string(" ab"), hex65}) {
        EXPECT_EQ(parse_socket2_line("openwindow>>" + bad + ",1,c,t"), std::nullopt) << bad;
        EXPECT_EQ(parse_socket2_line("closewindow>>" + bad), std::nullopt) << bad;
        EXPECT_EQ(parse_socket2_line("urgent>>" + bad), std::nullopt) << bad;
        EXPECT_EQ(parse_socket2_line("activewindowv2>>" + bad), std::nullopt)
            << bad << ": an invalid address is not 'nothing focused'";
    }
    EXPECT_EQ(parse_socket2_line("activewindowv2>>"), HyprEvent{hypr::ActiveWindowV2{""}})
        << "empty still means nothing is focused";
    EXPECT_EQ(parse_socket2_line("activewindowv2>>,"), HyprEvent{hypr::ActiveWindowV2{""}})
        << "an empty first field is empty too";
}

TEST(HyprlandParse, SubmapStillParses) {
    EXPECT_EQ(parse_socket2_line("submap>>resize"), HyprEvent{hypr::Submap{"resize"}});
    EXPECT_EQ(parse_socket2_line("submap>>"), HyprEvent{hypr::Submap{std::nullopt}})
        << "an empty name is the default submap";
    EXPECT_EQ(parse_socket2_line("submap>>a,b >> c"), HyprEvent{hypr::Submap{"a,b >> c"}})
        << "the name is everything after the first >>";
}

TEST(HyprlandTracker, FirstWorkspaceEventOnlyRecordsTheId) {
    HyprlandTracker t;
    EXPECT_EQ(t.on(hypr::WorkspaceV2{2}), (Out{check()})) << "no WorkspaceSwitched without a from";
    EXPECT_EQ(t.on(hypr::WorkspaceV2{2}), (Out{check()})) << "the same workspace is no switch";
    EXPECT_EQ(t.on(hypr::WorkspaceV2{3}), (Out{sig(signal::WorkspaceSwitched{2, 3}), check()}));
}

TEST(HyprlandTracker, WorkspaceSwitchEmitsFromAndToAndSpecialIdsAreIgnored) {
    HyprlandTracker t;
    (void)t.on(hypr::WorkspaceV2{1});
    EXPECT_EQ(t.on(hypr::WorkspaceV2{2}), (Out{sig(signal::WorkspaceSwitched{1, 2}), check()}));
    EXPECT_EQ(t.on(hypr::WorkspaceV2{-99}), Out{}) << "a special workspace";
    EXPECT_EQ(t.on(hypr::WorkspaceV2{0}), Out{});
    EXPECT_EQ(t.on(hypr::WorkspaceV2{2}), (Out{check()}))
        << "back from the special workspace is no switch";
    EXPECT_EQ(t.on(hypr::WorkspaceV2{5}), (Out{sig(signal::WorkspaceSwitched{2, 5}), check()}))
        << "the special id never became the 'from'";
}

TEST(HyprlandTracker, OpenAndCloseWindowEmitPulses) {
    HyprlandTracker t;
    EXPECT_EQ(t.on(hypr::OpenWindow{"a1"}), (Out{sig(signal::WindowOpened{})}));
    EXPECT_EQ(t.on(hypr::CloseWindow{"a1"}), (Out{sig(signal::WindowClosed{}), check()}));
    EXPECT_EQ(t.on(hypr::CloseWindow{"never-seen"}), (Out{sig(signal::WindowClosed{}), check()}));
}

TEST(HyprlandTracker, UrgentIsIgnoredForTheFocusedWindow) {
    HyprlandTracker t;
    EXPECT_EQ(t.on(hypr::Urgent{"a1"}), (Out{sig(signal::UrgentRaised{"a1"})}))
        << "nothing is known to be focused yet";
    focus(t, "kitty", "a1");
    EXPECT_EQ(t.on(hypr::Urgent{"a1"}), Out{}) << "the focused window needs no attention light";
    EXPECT_EQ(t.on(hypr::Urgent{"a2"}), (Out{sig(signal::UrgentRaised{"a2"})}));
}

TEST(HyprlandTracker, UrgentRaisesUntilFocusedOrClosed) {
    HyprlandTracker t;
    EXPECT_EQ(t.on(hypr::Urgent{"a2"}), (Out{sig(signal::UrgentRaised{"a2"})}));

    // Focusing it ends its urgency: closing it later clears nothing.
    (void)t.on(hypr::ActiveWindow{"mpv"});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a2"}),
              (Out{sig(signal::FocusChanged{"a2", "mpv"}), check()}));
    EXPECT_EQ(t.on(hypr::CloseWindow{"a2"}), (Out{sig(signal::WindowClosed{}), check()}))
        << "a2 was focused, so it is no longer urgent";

    // Closing an urgent window clears it, once.
    EXPECT_EQ(t.on(hypr::Urgent{"a3"}), (Out{sig(signal::UrgentRaised{"a3"})}));
    EXPECT_EQ(t.on(hypr::CloseWindow{"a3"}),
              (Out{sig(signal::WindowClosed{}), sig(signal::UrgentCleared{"a3"}), check()}));
    EXPECT_EQ(t.on(hypr::CloseWindow{"a3"}), (Out{sig(signal::WindowClosed{}), check()}));

    // Urgent again, after the focus moved away from a2: raised again.
    focus(t, "kitty", "a1");
    EXPECT_EQ(t.on(hypr::Urgent{"a2"}), (Out{sig(signal::UrgentRaised{"a2"})}));
}

TEST(HyprlandTracker, FocusChangedCarriesTheClassFromThePrecedingActiveWindowLine) {
    HyprlandTracker t;
    EXPECT_EQ(t.on(hypr::ActiveWindow{"firefox"}), Out{}) << "the class is only remembered";
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a1"}),
              (Out{sig(signal::FocusChanged{"a1", "firefox"}), check()}));
    EXPECT_EQ(t.on(hypr::ActiveWindow{"Alacritty"}), Out{});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a2"}),
              (Out{sig(signal::FocusChanged{"a2", "Alacritty"}), check()}));
    // Nothing focused: an empty class and an empty address.
    (void)t.on(hypr::ActiveWindow{""});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{""}), (Out{sig(signal::FocusChanged{"", ""}), check()}));
}

// T14-1: the class from an `activewindow` line belongs to the `activewindowv2` that follows it,
// once. If a line is lost (a title over 64 KiB is dropped whole), the next focus carries no class
// rather than the previous window's, and `replay` still knows the focused window's own class.
TEST(HyprlandTracker, AClassIsUsedByOneFocusChangeOnly) {
    using Signals = std::vector<Signal>;
    HyprlandTracker t;
    (void)t.on(hypr::ActiveWindow{"firefox"});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a1"}),
              (Out{sig(signal::FocusChanged{"a1", "firefox"}), check()}));
    EXPECT_EQ(t.replay(), (Signals{signal::FocusChanged{"a1", "firefox"}}));

    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"b2"}), (Out{sig(signal::FocusChanged{"b2", ""}), check()}))
        << "no activewindow line came for b2: its class is unknown, not firefox";
    EXPECT_EQ(t.replay(), (Signals{signal::FocusChanged{"b2", ""}}));

    // A repeat for the focused window (a title change) uses up its class line too.
    (void)t.on(hypr::ActiveWindow{"kitty"});
    (void)t.on(hypr::ActiveWindowV2{"c3"});
    (void)t.on(hypr::ActiveWindow{"kitty"});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"c3"}), Out{}) << "P15";
    EXPECT_EQ(t.replay(), (Signals{signal::FocusChanged{"c3", "kitty"}}))
        << "the focused window keeps its class";
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"d4"}), (Out{sig(signal::FocusChanged{"d4", ""}), check()}))
        << "the repeat's class line is not left over for the next window";
}

TEST(HyprlandTracker, FocusWorkspaceFullscreenAndCloseRequestAFullscreenCheck) {
    HyprlandTracker t;
    const auto has_check = [&t](const HyprEvent &e) {
        const Out out = t.on(e);
        return std::ranges::find(out, check()) != out.end();
    };
    EXPECT_TRUE(has_check(hypr::ActiveWindowV2{"a1"}));
    EXPECT_TRUE(has_check(hypr::WorkspaceV2{1}));
    EXPECT_TRUE(has_check(hypr::WorkspaceV2{2}));
    EXPECT_TRUE(has_check(hypr::Fullscreen{true}));
    EXPECT_TRUE(has_check(hypr::Fullscreen{false}));
    EXPECT_TRUE(has_check(hypr::CloseWindow{"a1"}));
    // Events that cannot change the fullscreen state ask for nothing.
    EXPECT_FALSE(has_check(hypr::OpenWindow{"a2"}));
    EXPECT_FALSE(has_check(hypr::Urgent{"a2"}));
    EXPECT_FALSE(has_check(hypr::ActiveWindow{"kitty"}));
    EXPECT_FALSE(has_check(hypr::Submap{"resize"}));
    EXPECT_FALSE(has_check(hypr::WorkspaceV2{-99}));
}

TEST(HyprlandTracker, SubmapPassesThrough) {
    HyprlandTracker t;
    EXPECT_EQ(t.on(hypr::Submap{"resize"}), (Out{TrackerOut{event::Submap{"resize"}}}));
    EXPECT_EQ(t.on(hypr::Submap{std::nullopt}), (Out{submap_cleared()}));
}

TEST(HyprlandTracker, DisconnectClearsUrgentFullscreenAndSubmap) {
    HyprlandTracker t;
    (void)t.on(hypr::WorkspaceV2{1});
    focus(t, "kitty", "a1");
    (void)t.on(hypr::Urgent{"a2"});
    EXPECT_EQ(t.on(hypr::Disconnected{}),
              (Out{sig(signal::UrgentReset{}), fullscreen_cleared(), submap_cleared(),
                   sig(signal::FocusChanged{"", ""})}))
        << "T14-4: the focus (and its class) is forgotten downstream too";
    EXPECT_EQ(t.replay(), std::vector<Signal>{}) << "and no focus is held to replay";
    // The state is gone too: the old urgent window clears nothing, the first workspace only
    // records, and the old focus emits again.
    EXPECT_EQ(t.on(hypr::CloseWindow{"a2"}), (Out{sig(signal::WindowClosed{}), check()}));
    EXPECT_EQ(t.on(hypr::WorkspaceV2{2}), (Out{check()}));
    (void)t.on(hypr::ActiveWindow{"kitty"});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a1"}),
              (Out{sig(signal::FocusChanged{"a1", "kitty"}), check()}));
}

TEST(HyprlandTracker, ConnectedResetsStateAndChecksFullscreen) {
    HyprlandTracker t;
    (void)t.on(hypr::WorkspaceV2{1});
    focus(t, "kitty", "a1");
    (void)t.on(hypr::Urgent{"a2"});
    EXPECT_EQ(t.on(hypr::Connected{}), (Out{check()}));
    EXPECT_EQ(t.on(hypr::WorkspaceV2{4}), (Out{check()})) << "the first workspace only records";
    EXPECT_EQ(t.on(hypr::CloseWindow{"a2"}), (Out{sig(signal::WindowClosed{}), check()}))
        << "the urgent set was emptied";
    EXPECT_EQ(t.on(hypr::Urgent{"a1"}), (Out{sig(signal::UrgentRaised{"a1"})}))
        << "no window is known to be focused any more";
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a1"}), (Out{sig(signal::FocusChanged{"a1", ""}), check()}))
        << "the old focus is forgotten, and so is the class remembered before";
}

// A title change of the focused window re-emits both focus events with the same address (17 pairs
// in 20 s on a real session). Without this the daemon would query hyprctl about once a second.
TEST(HyprlandTracker, RepeatedActiveWindowForTheSameWindowIsIgnored) {
    HyprlandTracker t;
    (void)t.on(hypr::ActiveWindow{"kitty"});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a1"}),
              (Out{sig(signal::FocusChanged{"a1", "kitty"}), check()}))
        << "one FocusChanged for the first focus";
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(t.on(hypr::ActiveWindow{"kitty"}), Out{});
        EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a1"}), Out{})
            << "repeat " << i << ": no FocusChanged and no fullscreen check";
    }
    (void)t.on(hypr::ActiveWindow{"mpv"});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a2"}),
              (Out{sig(signal::FocusChanged{"a2", "mpv"}), check()}));
    (void)t.on(hypr::ActiveWindow{"kitty"});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a1"}),
              (Out{sig(signal::FocusChanged{"a1", "kitty"}), check()}))
        << "going back to a1 is a change";
    (void)t.on(hypr::ActiveWindow{"kitty"});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a1"}), Out{});

    EXPECT_EQ(t.on(hypr::Connected{}), (Out{check()}));
    (void)t.on(hypr::ActiveWindow{"kitty"});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a1"}),
              (Out{sig(signal::FocusChanged{"a1", "kitty"}), check()}))
        << "after Connected the first focus event emits again";

    EXPECT_EQ(t.on(hypr::Disconnected{}),
              (Out{sig(signal::UrgentReset{}), fullscreen_cleared(), submap_cleared(),
                   sig(signal::FocusChanged{"", ""})}));
    (void)t.on(hypr::ActiveWindow{"kitty"});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a1"}),
              (Out{sig(signal::FocusChanged{"a1", "kitty"}), check()}))
        << "and after Disconnected";
}

// `replay` says what the tracker knows right now as the signals that would re-establish it: the
// focus first, then each urgent window in address order. It is a read: nothing changes.
TEST(HyprlandTracker, ReplayReportsTheHeldFocusAndUrgentWindows) {
    using Signals = std::vector<Signal>;
    HyprlandTracker t;
    EXPECT_EQ(t.replay(), Signals{}) << "nothing is known before any event";

    focus(t, "kitty", "a1");
    EXPECT_EQ(t.replay(), (Signals{signal::FocusChanged{"a1", "kitty"}}));

    // Raised out of order, so the set's own iteration order cannot pass for a sorted one.
    (void)t.on(hypr::Urgent{"u2"});
    (void)t.on(hypr::Urgent{"u1"});
    (void)t.on(hypr::Urgent{"u3"});
    EXPECT_EQ(t.replay(), (Signals{signal::FocusChanged{"a1", "kitty"}, signal::UrgentRaised{"u1"},
                                   signal::UrgentRaised{"u2"}, signal::UrgentRaised{"u3"}}))
        << "the focus first, then the urgent windows in sorted order";

    // Pure: asking twice gives the same answer, and the state behind it is untouched.
    EXPECT_EQ(t.replay(), t.replay());
    EXPECT_EQ(t.on(hypr::ActiveWindow{"kitty"}), Out{});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{"a1"}), Out{})
        << "the focus is still held, so a repeat is ignored";
    EXPECT_EQ(t.on(hypr::CloseWindow{"u2"}),
              (Out{sig(signal::WindowClosed{}), sig(signal::UrgentCleared{"u2"}), check()}))
        << "u2 is still in the urgent set";
    (void)t.on(hypr::Urgent{"u2"}); // put it back

    // Focusing an urgent window ends its urgency, so it is not replayed any more.
    focus(t, "mpv", "u1");
    EXPECT_EQ(t.replay(), (Signals{signal::FocusChanged{"u1", "mpv"}, signal::UrgentRaised{"u2"},
                                   signal::UrgentRaised{"u3"}}));

    // Nothing focused is a focus like any other: it replays as the empty address.
    focus(t, "", "");
    EXPECT_EQ(t.replay(), (Signals{signal::FocusChanged{"", ""}, signal::UrgentRaised{"u2"},
                                   signal::UrgentRaised{"u3"}}));

    // A (re)connect and a disconnect forget everything, so there is nothing to replay.
    (void)t.on(hypr::Connected{});
    EXPECT_EQ(t.replay(), Signals{});
    focus(t, "kitty", "a1");
    (void)t.on(hypr::Urgent{"u1"});
    EXPECT_EQ(t.replay(),
              (Signals{signal::FocusChanged{"a1", "kitty"}, signal::UrgentRaised{"u1"}}));
    (void)t.on(hypr::Disconnected{});
    EXPECT_EQ(t.replay(), Signals{});
}

// An empty address means "nothing focused". The first event after a reset always emits (even for
// nothing), and a repeat of "nothing focused" is ignored like any other repeat.
TEST(HyprlandTracker, NothingFocusedIsAFocusStateLikeAnyOther) {
    HyprlandTracker t;
    (void)t.on(hypr::ActiveWindow{""});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{""}), (Out{sig(signal::FocusChanged{"", ""}), check()}))
        << "the first event after construction emits, even with no window";
    (void)t.on(hypr::ActiveWindow{""});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{""}), Out{});
    focus(t, "kitty", "a1");
    (void)t.on(hypr::ActiveWindow{""});
    EXPECT_EQ(t.on(hypr::ActiveWindowV2{""}), (Out{sig(signal::FocusChanged{"", ""}), check()}))
        << "the focused window went away";
}
