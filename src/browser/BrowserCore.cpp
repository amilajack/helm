// SPDX-License-Identifier: GPL-3.0-or-later
// Embedded EM_ASM JavaScript must not be reformatted as C++.
// clang-format off
// Included in JUCE's core translation unit after its POSIX helpers.
#include "juce_core/native/juce_CommonFile_linux.cpp"
#include "juce_core/native/juce_Threads_linux.cpp"
namespace juce
{
File File::getSpecialLocation(SpecialLocationType type)
{
    switch (type)
    {
    case tempDirectory:
        return File("/tmp");
    case currentExecutableFile:
    case currentApplicationFile:
    case invokedExecutableFile:
        return File("/helm.js");
    case commonApplicationDataDirectory:
        return File("/factory");
    default:
        return File("/user");
    }
}
bool File::isOnCDRomDrive() const { return false; }
bool File::isOnHardDisk() const { return false; }
bool File::isOnRemovableDrive() const { return false; }
String File::getVersion() const { return {}; }
bool File::moveToTrash() const { return deleteFile(); }
void File::revealToUser() const {}
bool Process::openDocument(const String &url, const String &)
{
    if (!url.startsWith("https://") && !url.startsWith("http://"))
        return false;
    return EM_ASM_INT(
        { return window.open(UTF8ToString($0), '_blank', 'noopener') !== null; }, url.toRawUTF8());
}
} // namespace juce
// Direct UDP is a desktop-only integration. Fail explicitly instead of routing
// it through Emscripten's WebSocket socket emulation or a companion service.
namespace juce
{
DatagramSocket::DatagramSocket(bool, const SocketOptions &o) : options(o) {}
DatagramSocket::~DatagramSocket() = default;
bool DatagramSocket::bindToPort(int) { return false; }
bool DatagramSocket::bindToPort(int, const String &) { return false; }
int DatagramSocket::getBoundPort() const noexcept { return -1; }
int DatagramSocket::waitUntilReady(bool, int) { return -1; }
int DatagramSocket::read(void *, int, bool) { return -1; }
int DatagramSocket::read(void *, int, bool, String &, int &) { return -1; }
int DatagramSocket::write(const String &, int, const void *, int) { return -1; }
void DatagramSocket::shutdown() {}
bool DatagramSocket::joinMulticast(const String &) { return false; }
bool DatagramSocket::leaveMulticast(const String &) { return false; }
bool DatagramSocket::setMulticastLoopbackEnabled(bool) { return false; }
bool DatagramSocket::setEnablePortReuse(bool) { return false; }
} // namespace juce

// clang-format on
// Called by the JUCE FreeType overlay when no loaded face has a glyph. The page
// fetches the fallback fonts once and calls helm_browser_fonts_added.
extern "C" void helm_browser_request_fallback_fonts()
{
    EM_ASM({ globalThis.HelmBrowser?.requestFallbackFonts?.(); });
}

// Browsers have asynchronous fetch, provided by HelmLibrary. Native blocking
// URL streams are unavailable; fail explicitly rather than blocking the UI.
namespace juce {
class WebInputStream::Pimpl {};
WebInputStream::WebInputStream(const URL&, bool) {}
WebInputStream::~WebInputStream() = default;
bool WebInputStream::Listener::postDataSendProgress(WebInputStream&,int,int) {return false;}
WebInputStream& WebInputStream::withExtraHeaders(const String&) { return *this; }
WebInputStream& WebInputStream::withCustomRequestCommand(const String&) { return *this; }
WebInputStream& WebInputStream::withConnectionTimeout(int) { return *this; }
WebInputStream& WebInputStream::withNumRedirectsToFollow(int) { return *this; }
StringPairArray WebInputStream::getRequestHeaders() const { return {}; }
StringPairArray WebInputStream::getResponseHeaders() { return {}; }
bool WebInputStream::connect(Listener*) { return false; }
bool WebInputStream::isError() const { return true; }
void WebInputStream::cancel() {}
bool WebInputStream::isExhausted() { return true; }
int64 WebInputStream::getPosition() { return 0; }
int64 WebInputStream::getTotalLength() { return -1; }
int WebInputStream::read(void*,int) { return 0; }
bool WebInputStream::setPosition(int64) { return false; }
int WebInputStream::getStatusCode() { return 0; }
}
