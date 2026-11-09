#include "TestHarness.h"

#include "edge/SubscriptionPolicy.h"

using namespace corevideo::edge;

namespace {
std::vector<RosterEntry> roster(int count, std::uint32_t firstId = 101) {
  std::vector<RosterEntry> r;
  for (int i = 0; i < count; ++i) r.push_back({firstId + i, "Guest " + std::to_string(i + 1), false});
  return r;
}
}  // namespace

TEST(SubscriptionPolicy, NothingIsSubscribedUnlessAsked) {
  SubscriptionBook book;
  std::vector<SubscriptionAction> actions;
  book.applyRoster(roster(12), actions);
  EXPECT_TRUE(actions.empty());
  EXPECT_TRUE(book.selections().empty());
}

TEST(SubscriptionPolicy, DefaultCapIsSixAndIsEnforced) {
  SubscriptionBook book;
  EXPECT_EQ(book.cap(), 6);
  const auto people = roster(7);
  std::vector<SubscriptionAction> actions;
  for (int i = 0; i < 6; ++i) EXPECT_EQ(book.request(people[i], true, actions), std::string());
  const auto refusal = book.request(people[6], true, actions);
  EXPECT_FALSE(refusal.empty());
  EXPECT_EQ(book.selections().size(), size_t{6});
  EXPECT_EQ(actions.size(), size_t{6});
}

TEST(SubscriptionPolicy, CapCannotExceedTheEngineIsoTable) {
  SubscriptionBook book(50);
  EXPECT_EQ(book.cap(), kHardMaxSubscriptions);
  book.setCap(0);
  EXPECT_EQ(book.cap(), 1);
}

TEST(SubscriptionPolicy, NeverSubscribesItself) {
  SubscriptionBook book;
  std::vector<SubscriptionAction> actions;
  EXPECT_FALSE(book.request({99, "CoreVideo Edge", true}, true, actions).empty());
  EXPECT_TRUE(actions.empty());
}

TEST(SubscriptionPolicy, SlotsAreStableAndReused) {
  SubscriptionBook book;
  const auto people = roster(3);
  std::vector<SubscriptionAction> actions;
  book.request(people[0], true, actions);
  book.request(people[1], true, actions);
  book.request(people[2], true, actions);
  EXPECT_EQ(book.findByParticipant(102)->slot, 2);
  book.request(people[1], false, actions);
  EXPECT_EQ(actions.back().kind, SubscriptionAction::Kind::Unsubscribe);
  EXPECT_EQ(actions.back().slot, 2);
  book.request({200, "Late", false}, true, actions);
  EXPECT_EQ(book.findByParticipant(200)->slot, 2);
  EXPECT_EQ(slotSourceUuid(2), std::string("edge-s2"));
}

TEST(SubscriptionPolicy, ARejoinedGuestKeepsTheirSlot) {
  SubscriptionBook book;
  std::vector<SubscriptionAction> actions;
  book.request({101, "Alice", false}, true, actions);
  actions.clear();
  // Alice drops: slot held, nothing sent.
  book.applyRoster({{102, "Bob", false}}, actions);
  EXPECT_TRUE(actions.empty());
  EXPECT_FALSE(book.findBySlot(1)->present);
  // Alice returns with a new Zoom id: same slot, re-subscribed.
  book.applyRoster({{102, "Bob", false}, {555, "Alice", false}}, actions);
  ASSERT_TRUE(actions.size() == 1);
  EXPECT_EQ(actions[0].slot, 1);
  EXPECT_EQ(actions[0].participantId, 555u);
  EXPECT_EQ(actions[0].reason, std::string("rejoined"));
  EXPECT_TRUE(book.findBySlot(1)->present);
}

TEST(SubscriptionPolicy, ConfigNamesAreSubscribedWhenPresentAndRespectTheCap) {
  SubscriptionBook book(2);
  book.setConfigNames({"guest 1", "Guest 2", "Guest 3"});
  std::vector<SubscriptionAction> actions;
  book.applyRoster(roster(5), actions);
  EXPECT_EQ(actions.size(), size_t{2});
  EXPECT_TRUE(book.findByParticipant(101) && book.findByParticipant(101)->fromConfig);
  EXPECT_FALSE(book.configWarning().empty());
  actions.clear();
  book.applyRoster(roster(5), actions);  // idempotent
  EXPECT_TRUE(actions.empty());
}

TEST(SubscriptionPolicy, IsoDefaultsOnAndCanBeToggled) {
  SubscriptionBook book;
  std::vector<SubscriptionAction> actions;
  book.request({101, "A", false}, true, actions);
  EXPECT_TRUE(book.findBySlot(1)->iso);
  EXPECT_EQ(book.setIso(1, false), std::string());
  EXPECT_FALSE(book.findBySlot(1)->iso);
  EXPECT_FALSE(book.setIso(7, true).empty());
}
