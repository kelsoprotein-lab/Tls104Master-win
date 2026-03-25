#ifndef WEBVIEW_WINDOW_H
#define WEBVIEW_WINDOW_H

#include <string>

namespace tls104 {

/// Launch a native webview window. Blocks until the window is closed.
void runWebviewWindow(const std::string& title, const std::string& url,
                      int width = 1280, int height = 800);

} // namespace tls104

#endif // WEBVIEW_WINDOW_H
