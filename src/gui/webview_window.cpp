#include "gui/webview_window.h"
#include "webview/webview.h"

namespace tls104 {

void runWebviewWindow(const std::string& title, const std::string& url,
                      int width, int height) {
    webview::webview w(false, nullptr);
    w.set_title(title);
    w.set_size(width, height, WEBVIEW_HINT_NONE);
    w.navigate(url);
    w.run();
}

} // namespace tls104
