#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <dv/core/result.hpp>

namespace {

TEST(Result, HoldsValueOnSuccess) {
  const dv::Result<int> result = 42;
  EXPECT_TRUE(result.ok());
  EXPECT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(result.value(), 42);
}

TEST(Result, HoldsErrorOnFailure) {
  const auto result = dv::Result<int>::failure("bad_input", "not a number");
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(result.error().code, "bad_input");
  EXPECT_EQ(result.error().message, "not a number");
}

TEST(Result, ValueOnFailureIsAProgrammingError) {
  const auto result = dv::Result<int>::failure("bad_input", "not a number");
  EXPECT_THROW((void)result.value(), std::logic_error);
}

TEST(Result, ErrorOnSuccessIsAProgrammingError) {
  const dv::Result<int> result = 1;
  EXPECT_THROW((void)result.error(), std::logic_error);
}

TEST(Result, TakeMovesTheValueOut) {
  dv::Result<std::string> result = std::string(1000, 'x');
  const std::string moved = std::move(result).take();
  EXPECT_EQ(moved.size(), 1000u);
}

TEST(Result, ValueOrReturnsFallbackOnFailure) {
  const auto failed = dv::Result<int>::failure("nope", "");
  EXPECT_EQ(failed.value_or(7), 7);

  const dv::Result<int> succeeded = 3;
  EXPECT_EQ(succeeded.value_or(7), 3);
}

}  // namespace
