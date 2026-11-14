// The entry point of the interface tests.
//
// Its own main and not gtest_main, because a widget cannot exist without a
// QApplication and there is one per process: it is made here, once, before
// the first test, and every test builds its widgets under it.
//
// No window ever reaches a screen. The offscreen platform plugin gives Qt
// somewhere to draw that is nowhere, which is what lets this run on a build
// machine with no display - and on a developer's, without windows flashing
// up and taking the focus from whoever is at the keyboard.

#include <cstdio>
#include <exception>

#include <gtest/gtest.h>

#include <QApplication>
#include <QtEnvironmentVariables>

#include "ui/theme.hpp"

int main(int argc, char* argv[]) {
  // Nothing is allowed to leave main: see client/src/main.cpp.
  try {
    // Unless somebody asked for a real one, to watch a test happen.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
      qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    ::testing::InitGoogleTest(&argc, argv);

    QApplication application(argc, argv);
    // Off for every test. An animation is a thing that finishes later, and a
    // test that has to wait for later is a test that sometimes fails.
    dv::ui::theme::motion::set_enabled(false);
    dv::ui::theme::apply(application, dv::ui::theme::Mode::Dark);
    return RUN_ALL_TESTS();
  } catch (const std::exception& error) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    std::fprintf(stderr, "fatal: %s\n", error.what());
    return 1;
  }
}
