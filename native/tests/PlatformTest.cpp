#include "TestHarness.h"

#include "edge/ShmReopen.h"

#include "engine-ipc.h"

#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace corevideo::edge;

namespace {
unsigned long pid() {
#if defined(_WIN32)
  return static_cast<unsigned long>(GetCurrentProcessId());
#else
  return static_cast<unsigned long>(getpid());
#endif
}
std::uint32_t sequence(const ShmRegion& r) { return static_cast<const volatile ShmFrameHeader*>(r.ptr)->sequence; }
}  // namespace

TEST(ShmReopen, AnIdleRegionIsReopenedOnlyOnPosix) {
  ShmWatch watch;
  EXPECT_FALSE(watch.shouldReopen(2, 1000));
  EXPECT_FALSE(watch.shouldReopen(2, 1000 + kShmReopenAfterMs - 1));
#if defined(_WIN32)
  EXPECT_FALSE(watch.shouldReopen(2, 1000 + kShmReopenAfterMs));
#else
  EXPECT_TRUE(watch.shouldReopen(2, 1000 + kShmReopenAfterMs));
  EXPECT_FALSE(watch.shouldReopen(2, 1000 + kShmReopenAfterMs + 1));  // the interval restarts
#endif
  EXPECT_FALSE(watch.shouldReopen(4, 5000));  // progress: never
}

// The engine destroys and re-creates a region under the same name (unsubscribe
// then subscribe, Engine Off/On, a room move). Windows readers keep reading
// the live section; POSIX readers keep the orphan until they reopen.
TEST(ShmReopen, AReaderFollowsARecreatedRegion) {
  const std::string name = "EdgeTest_" + std::to_string(pid()) + "_reopen";
  ShmRegion writer, reader;
  ASSERT_TRUE(shm_region_create(writer, name, 4096));
  static_cast<ShmFrameHeader*>(writer.ptr)->sequence = 2;
  ASSERT_TRUE(shm_region_open_read(reader, name, 4096));
  EXPECT_EQ(sequence(reader), std::uint32_t{2});

  shm_region_destroy(writer);
  ASSERT_TRUE(shm_region_create(writer, name, 4096));
  static_cast<ShmFrameHeader*>(writer.ptr)->sequence = 4;
#if defined(_WIN32)
  EXPECT_EQ(sequence(reader), std::uint32_t{4});  // same section
#else
  EXPECT_EQ(sequence(reader), std::uint32_t{2});  // the orphan
  ShmWatch watch;
  (void)watch.shouldReopen(sequence(reader), 0);
  if (watch.shouldReopen(sequence(reader), kShmReopenAfterMs)) {
    shm_region_destroy(reader);
    ASSERT_TRUE(shm_region_open_read(reader, name, 4096));
  }
  EXPECT_EQ(sequence(reader), std::uint32_t{4});
#endif
  shm_region_destroy(reader);
  shm_region_destroy(writer);
}
