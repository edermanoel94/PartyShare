#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

/// The version this program is, and the arithmetic for deciding that another
/// one is newer.
///
/// There is exactly one number, the `project(VERSION ...)` call in the top
/// level CMakeLists.txt, which shared/CMakeLists.txt hands to the compiler as
/// DV_VERSION and which .github/workflows/tag.yml writes back into that line
/// before it creates the tag `vX.Y.Z`. So the string compiled into this binary
/// and the name of the tag that produced it are the same three numbers, and
/// comparing a published tag against a running build is comparing two of these.
///
/// Header only, like dv/core/result.hpp beside it and for the same reason: it
/// is small, it is constexpr, and being constexpr is what lets the static
/// assertion at the bottom of this file prove at compile time that DV_VERSION
/// parses. A version string nobody can read would otherwise become a client
/// that believes every release is newer than itself.
namespace dv::core {

/// A release: the three numbers of its tag, and nothing else.
///
/// Not full semantic versioning. There is no pre-release suffix and no build
/// metadata here because this project has never published one - the tag job
/// only ever writes MAJOR.MINOR.PATCH - and GitHub's "latest release" endpoint
/// excludes pre-releases and drafts anyway. A tag shaped like anything else is
/// refused by the parser rather than half understood, which errs towards saying
/// nothing to the user.
struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;

  /// Ordering is the whole point of this type, and the default is the right
  /// one: major first, then minor, then patch, which is what the numbers mean.
  friend constexpr auto operator<=>(const Version&, const Version&) = default;
  friend constexpr bool operator==(const Version&, const Version&) = default;

  [[nodiscard]] std::string to_string() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
  }
};

namespace detail {

/// One of the three numbers, or nothing when `text` is not a plain decimal
/// number small enough to hold.
///
/// Nine digits is the ceiling, which is four more than any of these will ever
/// need and two short of overflowing an int. Written out rather than left to
/// std::from_chars because that is not constexpr before C++23, and being
/// usable in a constant expression is what the assertion at the bottom needs.
constexpr std::optional<int> parse_component(std::string_view text) {
  if (text.empty() || text.size() > 9) {
    return std::nullopt;
  }
  int value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return std::nullopt;
    }
    value = value * 10 + (digit - '0');
  }
  return value;
}

}  // namespace detail

/// `text` read as MAJOR.MINOR.PATCH, or nothing when it is not one.
///
/// A single leading `v` is accepted, and is why this is not three calls to a
/// number parser at the call site: what arrives from GitHub is the tag name,
/// `v0.1.41`, and what is compiled in is the bare `0.1.41`. Both have to reach
/// the same Version or the client compares a number against a number that
/// happens to have a letter in front of it and finds them different forever.
///
/// Everything else is refused: two components, four components, a suffix, an
/// empty component, a space. Refusing is the safe direction - an unreadable tag
/// produces no notice, where a generously read one produces a notice that
/// cannot be dismissed by updating.
[[nodiscard]] constexpr std::optional<Version> parse_version(std::string_view text) {
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
    text.remove_prefix(1);
  }

  const std::size_t first = text.find('.');
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t second = text.find('.', first + 1);
  if (second == std::string_view::npos) {
    return std::nullopt;
  }
  if (text.find('.', second + 1) != std::string_view::npos) {
    return std::nullopt;
  }

  const std::optional<int> major = detail::parse_component(text.substr(0, first));
  const std::optional<int> minor =
      detail::parse_component(text.substr(first + 1, second - first - 1));
  const std::optional<int> patch = detail::parse_component(text.substr(second + 1));
  if (!major || !minor || !patch) {
    return std::nullopt;
  }
  return Version{.major = *major, .minor = *minor, .patch = *patch};
}

#ifdef DV_VERSION
/// What this build is.
///
/// The fallback is unreachable: the assertion below refuses to compile a
/// DV_VERSION this cannot read, and it fires on the machine that builds the
/// binary rather than on the machine that runs it.
///
/// It is written as value_or rather than as the dereference it plainly is
/// because bugprone-unchecked-optional-access reads the two lines separately -
/// it models the optional and not the static_assert underneath it - and the CI
/// treats that check as an error. Naming the impossible answer costs one word
/// and is worth more than a suppression comment: 0.0.0 is behind every release
/// there has ever been, so even the branch that cannot be taken would err
/// towards offering an update rather than towards hiding one.
[[nodiscard]] constexpr Version running_version() {
  return parse_version(DV_VERSION).value_or(Version{});
}

static_assert(parse_version(DV_VERSION).has_value(),
              "DV_VERSION is not MAJOR.MINOR.PATCH; the project() call in the top level "
              "CMakeLists.txt is what feeds it, and .github/workflows/tag.yml turns the same "
              "line into a tag, so a version this cannot read is a tag nothing can compare "
              "against");
#endif

/// Whether `published` is worth interrupting somebody running `running` about.
///
/// Strictly newer, which decides the one case that is not obvious: a build made
/// from master after the last tag - every developer's build, and every build
/// from a pull request - is *ahead* of the newest release, and is told nothing.
/// The alternative, treating "different" as "out of date", would put a notice
/// offering an older version in front of exactly the people who are working on
/// the newer one.
[[nodiscard]] constexpr bool is_update(const Version& published, const Version& running) {
  return published > running;
}

}  // namespace dv::core
