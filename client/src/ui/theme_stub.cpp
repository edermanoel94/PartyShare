#include "ui/theme.hpp"

namespace dv::ui::theme {

// macOS and the Linux desktops draw the title bar from the application's own
// appearance or from the window manager's, and neither is asked through a call
// like the one Windows has. Nothing to do, and nothing to report.
void platform_set_title_bar(QWidget* /*window*/, bool /*dark*/) {}

}  // namespace dv::ui::theme
