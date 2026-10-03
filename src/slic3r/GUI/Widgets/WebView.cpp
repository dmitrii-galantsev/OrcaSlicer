#include "WebView.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/TouchKeypad.hpp"
#include "slic3r/Utils/MacDarkMode.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <boost/log/trivial.hpp>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <exception>
#include <thread>

#include <wx/setup.h>
#include <wx/webview.h>
#include <wx/string.h>
#include <wx/gdicmn.h>
#include <wx/sharedptr.h>
#include <wx/vector.h>
#include <wx/event.h>
#include <vector>
#include <wx/object.h>
#include <wx/log.h>
#include <utility>
#include <wx/webviewarchivehandler.h>
#include <wx/webviewfshandler.h>
#include <wx/weakref.h>
#if wxUSE_WEBVIEW_EDGE
#include <wx/msw/webview_edge.h>
#elif defined(__WXMAC__)
#include <wx/osx/webview_webkit.h>
#endif
#include <wx/uri.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#if defined(__WIN32__) || defined(__WXMAC__)
#include "wx/private/jsscriptwrapper.h"
#endif

#ifdef __WIN32__
#include <WebView2.h>
#include <Shellapi.h>
#include <slic3r/Utils/Http.hpp>
#elif defined __linux__
#include <gtk/gtk.h>
#define WEBKIT_API
struct WebKitWebView;
struct WebKitJavascriptResult;
extern "C" {
WEBKIT_API void
webkit_web_view_run_javascript                       (WebKitWebView             *web_view,
                                                      const gchar               *script,
                                                      GCancellable              *cancellable,
                                                      GAsyncReadyCallback       callback,
                                                      gpointer                  user_data);
WEBKIT_API WebKitJavascriptResult *
webkit_web_view_run_javascript_finish                (WebKitWebView             *web_view,
                                                      GAsyncResult              *result,
						      GError                    **error);
WEBKIT_API void
webkit_javascript_result_unref              (WebKitJavascriptResult *js_result);
}
#endif

#ifdef __WIN32__
// Run Download and Install in another thread so we don't block the UI thread
DWORD DownloadAndInstallWV2RT() {

  int returnCode = 2; // Download failed
  // Use fwlink to download WebView2 Bootstrapper at runtime and invoke installation
  // Broken/Invalid Https Certificate will fail to download
  // Use of the download link below is governed by the below terms. You may acquire the link
  // for your use at https://developer.microsoft.com/microsoft-edge/webview2/. Microsoft owns
  // all legal right, title, and interest in and to the WebView2 Runtime Bootstrapper
  // ("Software") and related documentation, including any intellectual property in the
  // Software. You must acquire all code, including any code obtained from a Microsoft URL,
  // under a separate license directly from Microsoft, including a Microsoft download site
  // (e.g., https://developer.microsoft.com/microsoft-edge/webview2/).
  // HRESULT hr = URLDownloadToFileW(NULL, L"https://go.microsoft.com/fwlink/p/?LinkId=2124703",
  //                               L".\\plugin\\MicrosoftEdgeWebview2Setup.exe", 0, 0);
  fs::path target_file_path = (fs::temp_directory_path() / "MicrosoftEdgeWebview2Setup.exe");
  bool downloaded = false;
  Slic3r::Http::get("https://go.microsoft.com/fwlink/p/?LinkId=2124703")
      .on_error([](std::string body, std::string error, unsigned http_status) {

      })
      .on_complete([&downloaded, target_file_path](std::string body, unsigned http_status) {
        fs::fstream file(target_file_path, std::ios::out | std::ios::binary | std::ios::trunc);
        file.write(body.c_str(), body.size());
        file.flush();
        file.close();

        downloaded = true;
      })
      .perform_sync();
  // Sleep for 1 second to wait for the buffer writen into disk
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  if (downloaded) {
    // Either Package the WebView2 Bootstrapper with your app or download it using fwlink
    // Then invoke install at Runtime.
    // Keep the path string alive for the duration of the ShellExecuteExW call;
    // assigning .c_str() of a temporary directly would leave lpFile dangling.
    const std::wstring installer_path = target_file_path.generic_wstring();
    SHELLEXECUTEINFOW shExInfo = {0};
    shExInfo.cbSize = sizeof(shExInfo);
    shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    shExInfo.hwnd = 0;
    shExInfo.lpVerb = L"runas";
    shExInfo.lpFile = installer_path.c_str();
    shExInfo.lpParameters = L" /silent /install";
    shExInfo.lpDirectory = 0;
    shExInfo.nShow = 0;
    shExInfo.hInstApp = 0;

    if (ShellExecuteExW(&shExInfo)) {
      WaitForSingleObject(shExInfo.hProcess, INFINITE);
      returnCode = 0; // Install successfull
    } else {
      returnCode = 1; // Install failed
    }
  }
  return returnCode;
}

class WebViewEdge : public wxWebViewEdge
{
public:
    bool SetUserAgent(const wxString &userAgent) override
    {
        bool dark = userAgent.Contains("dark");
        SetColorScheme(dark ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT);

        ICoreWebView2 *webView2 = (ICoreWebView2 *) GetNativeBackend();
        if (webView2) {
            ICoreWebView2Settings *settings;
            HRESULT                hr = webView2->get_Settings(&settings);
            if (hr == S_OK) {
                ICoreWebView2Settings2 *settings2;
                hr = settings->QueryInterface(&settings2);
                if (hr == S_OK) {
                    settings2->put_UserAgent(userAgent.wc_str());
                    settings2->Release();
                    return true;
                }
            }
            settings->Release();
            return false;
        }
        pendingUserAgent = userAgent;
        return true;
    }

    bool SetColorScheme(COREWEBVIEW2_PREFERRED_COLOR_SCHEME colorScheme)
    {
        ICoreWebView2 *webView2 = (ICoreWebView2 *) GetNativeBackend();
        if (webView2) {
            ICoreWebView2_13 * webView2_13;
            HRESULT           hr = webView2->QueryInterface(&webView2_13);
            if (hr == S_OK) {
                ICoreWebView2Profile *profile;
                hr = webView2_13->get_Profile(&profile);
                if (hr == S_OK) {
                    profile->put_PreferredColorScheme(colorScheme);
                    profile->Release();
                    return true;
                }
                webView2_13->Release();
            }
            return false;
        }
        pendingColorScheme = colorScheme;
        return true;
    }

    void DoGetClientSize(int *x, int *y) const override
    {
        if (!pendingUserAgent.empty()) {
            auto thiz = const_cast<WebViewEdge *>(this);
            auto userAgent = std::move(thiz->pendingUserAgent);
            thiz->pendingUserAgent.clear();
            thiz->SetUserAgent(userAgent);
        }
        if (pendingColorScheme) {
            auto thiz      = const_cast<WebViewEdge *>(this);
            auto colorScheme = pendingColorScheme;
            thiz->pendingColorScheme = COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO;
            thiz->SetColorScheme(colorScheme);
        }
        wxWebViewEdge::DoGetClientSize(x, y);
    };
private:
    wxString pendingUserAgent;
    COREWEBVIEW2_PREFERRED_COLOR_SCHEME pendingColorScheme = COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO;
};

#elif defined __WXOSX__

class WebViewWebKit : public wxWebViewWebKit
{
public:
    WebViewWebKit()
        : wxWebViewWebKit(wxWebView::NewConfiguration(wxWebViewBackendWebKit))
    {
    }

    ~WebViewWebKit() override
    {
        RemoveScriptMessageHandler("wx");
    }
};

#endif

class FakeWebView : public wxWebView
{
    virtual bool Create(wxWindow* parent, wxWindowID id, const wxString& url, const wxPoint& pos, const wxSize& size, long style, const wxString& name) override { return false; }
    virtual wxString GetCurrentTitle() const override { return wxString(); }
    virtual wxString GetCurrentURL() const override { return wxString(); }
    virtual bool IsBusy() const override { return false; }
    virtual bool IsEditable() const override { return false; }
    virtual void LoadURL(const wxString& url) override { }
    virtual void Print() override { }
    virtual void RegisterHandler(wxSharedPtr<wxWebViewHandler> handler) override { }
    virtual void Reload(wxWebViewReloadFlags flags = wxWEBVIEW_RELOAD_DEFAULT) override { }
    virtual bool RunScript(const wxString& javascript, wxString* output = NULL) const override { return false; }
    virtual void SetEditable(bool enable = true) override { }
    virtual void Stop() override { }
    virtual bool CanGoBack() const override { return false; }
    virtual bool CanGoForward() const override { return false; }
    virtual void GoBack() override { }
    virtual void GoForward() override { }
    virtual void ClearHistory() override { }
    virtual void EnableHistory(bool enable = true) override { }
    virtual wxVector<wxSharedPtr<wxWebViewHistoryItem>> GetBackwardHistory() override { return {}; }
    virtual wxVector<wxSharedPtr<wxWebViewHistoryItem>> GetForwardHistory() override { return {}; }
    virtual void LoadHistoryItem(wxSharedPtr<wxWebViewHistoryItem> item) override { }
    virtual bool CanSetZoomType(wxWebViewZoomType type) const override { return false; }
    virtual float GetZoomFactor() const override { return 0.0f; }
    virtual wxWebViewZoomType GetZoomType() const override { return wxWebViewZoomType(); }
    virtual void SetZoomFactor(float zoom) override { }
    virtual void SetZoomType(wxWebViewZoomType zoomType) override { }
    virtual bool CanUndo() const override { return false; }
    virtual bool CanRedo() const override { return false; }
    virtual void Undo() override { }
    virtual void Redo() override { }
    virtual void* GetNativeBackend() const override { return nullptr; }
    virtual void DoSetPage(const wxString& html, const wxString& baseUrl) override { }
};

wxDEFINE_EVENT(EVT_WEBVIEW_RECREATED, wxCommandEvent);

static std::vector<wxWebView*> g_webviews;
// Webviews waiting for their script handler while another one is added; adding it yields, so a
// view can be destroyed while it waits.
static std::vector<wxWeakRef<wxWebView>> g_delay_webviews;
static std::vector<wxWeakRef<wxWebView>> g_delay_touch_keypads;

class WebViewRef : public wxObjectRefData
{
public:
    WebViewRef(wxWebView *webView) : m_webView(webView) {}
    ~WebViewRef() {
        auto iter = std::find(g_webviews.begin(), g_webviews.end(), m_webView);
        assert(iter != g_webviews.end());
        if (iter != g_webviews.end())
            g_webviews.erase(iter);
    }
    wxWebView *m_webView;
    // Guards against registering the "wx" handler twice (a duplicate throws on WKWebView).
    bool m_script_handler_added = false;
};

static WebViewRef *webview_ref(wxWebView *webView)
{
    return webView ? static_cast<WebViewRef *>(webView->GetRefData()) : nullptr;
}

wxWebView* WebView::CreateWebView(wxWindow * parent, wxString const & url)
{
#if wxUSE_WEBVIEW_EDGE
    // Check if a fixed version of edge is present in
    // $executable_path/edge_fixed and use it
    wxFileName edgeFixedDir(wxStandardPaths::Get().GetExecutablePath());
    edgeFixedDir.SetFullName("");
    edgeFixedDir.AppendDir("edge_fixed");
    if (edgeFixedDir.DirExists()) {
        wxWebViewEdge::MSWSetBrowserExecutableDir(edgeFixedDir.GetFullPath());
        wxLogMessage("Using fixed edge version");
    }
#endif
    auto url2  = url;
#ifdef __WIN32__
    url2.Replace("\\", "/");
#endif
    if (!url2.empty()) { url2 = wxURI(url2).BuildURI(); }
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << url2.ToUTF8();

#ifdef __WIN32__
    wxWebView* webView = new WebViewEdge;
#elif defined(__WXOSX__)
    wxWebView* webView = new WebViewWebKit;
#else
    auto webView = wxWebView::New();
#endif
    if (webView) {
        webView->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

        wxString language_code = Slic3r::GUI::wxGetApp().current_language_code().BeforeFirst('_');
        language_code          = language_code.ToStdString();
#ifdef __WIN32__
        webView->SetUserAgent(wxString::Format("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                               "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/107.0.0.0 Safari/537.36 Edg/107.0.1418.52 BBL-Slicer/v%s (%s) BBL-Language/%s",
                                               Slic3r::GUI::wxGetApp().get_bbl_client_version(), Slic3r::GUI::wxGetApp().dark_mode() ? "dark" : "light", language_code.mb_str()));
        webView->Create(parent, wxID_ANY, url2, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        // We register the wxfs:// protocol for testing purposes
        webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewArchiveHandler("bbl")));
        // And the memory: file system
        webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewFSHandler("memory")));
#else
        // With WKWebView handlers need to be registered before creation.
        // On Linux (WebKit2GTK), URI schemes are registered globally and can only
        // be registered once, so guard against multiple registrations.
        static bool s_schemes_registered = false;
        if (!s_schemes_registered) {
            webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewArchiveHandler("wxfs")));
            webView->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new wxWebViewFSHandler("memory")));
            s_schemes_registered = true;
        }
        webView->Create(parent, wxID_ANY, url2, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        webView->SetUserAgent(wxString::Format("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) BBL-Slicer/v%s (%s) BBL-Language/%s",
                                               Slic3r::GUI::wxGetApp().get_bbl_client_version(), Slic3r::GUI::wxGetApp().dark_mode() ? "dark" : "light", language_code.mb_str()));
#endif
#ifdef __WXMAC__
        WKWebView * wkWebView = (WKWebView *) webView->GetNativeBackend();
        Slic3r::GUI::WKWebView_setTransparentBackground(wkWebView);
#endif
        auto addScriptMessageHandler = [] (wxWebView *webView) {
            // Skip if SendAPIKey() already registered "wx"; a duplicate add throws an
            // uncatchable NSException on WKWebView, killing the app at startup.
            WebViewRef *ref = webview_ref(webView);
            if (ref && ref->m_script_handler_added)
                return;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": begin to add script message handler for wx.";
            Slic3r::GUI::wxGetApp().set_adding_script_handler(true);
            if (!webView->AddScriptMessageHandler("wx"))
                wxLogError("Could not add script message handler");
            else if (ref)
                ref->m_script_handler_added = true;
            Slic3r::GUI::wxGetApp().set_adding_script_handler(false);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": finished add script message handler for wx.";
        };
#ifndef __WIN32__
        webView->CallAfter([webView, addScriptMessageHandler] {
#endif
            if (Slic3r::GUI::wxGetApp().is_adding_script_handler()) {
                g_delay_webviews.push_back(webView);
            } else {
                addScriptMessageHandler(webView);
                while (!g_delay_webviews.empty()) {
                    auto views = std::move(g_delay_webviews);
                    for (const wxWeakRef<wxWebView>& wv : views)
                        if (wv)
                            addScriptMessageHandler(wv.get());
                }
                WebView::EnableDelayedTouchKeypads();
            }
#ifndef __WIN32__
        });
#endif
        // Deferred so the owner's message handler, bound right after CreateWebView returns, is
        // already in place: the handler bound last sees a message first.
        webView->CallAfter([webView] { WebView::EnableTouchKeypad(webView); });
        webView->EnableContextMenu(true);
    } else {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": failed. Use fake web view.";
        webView = new FakeWebView;
    }
    webView->SetRefData(new WebViewRef(webView));
    g_webviews.push_back(webView);
    return webView;
}

void WebView::MarkScriptMessageHandlerAdded(wxWebView * webView)
{
    if (WebViewRef *ref = webview_ref(webView))
        ref->m_script_handler_added = true;
}

static const char TOUCH_KEYPAD_HANDLER[] = "orcaTouchInput";

// Sets the value through the prototype setter so pages whose framework wraps the value property
// still see the change, and blurs the input so pages that commit on focus loss commit too.
static const char TOUCH_KEYPAD_SCRIPT[] = R"JS(
(function () {
    if (window.__orcaTouchKeypad)
        return;
    var textTypes = ['text', 'search', 'email', 'url', 'tel', 'password', 'number'];
    var target = null;
    var serial = 0;
    function editable(el) {
        if (!el || el.disabled || el.readOnly)
            return false;
        if (el.tagName === 'TEXTAREA')
            return true;
        return el.tagName === 'INPUT' && textTypes.indexOf(el.type) >= 0;
    }
    function post(message) {
        var handler = window.orcaTouchInput ||
            (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.orcaTouchInput);
        if (handler)
            handler.postMessage(JSON.stringify(message));
    }
    document.addEventListener('click', function (event) {
        var el = event.composedPath ? event.composedPath()[0] : event.target;
        if (!event.isTrusted || !editable(el))
            return;
        target = el;
        serial += 1;
        var mode = (el.getAttribute('inputmode') || '').toLowerCase();
        post({
            orca_touch_keypad: serial,
            value: el.value,
            numeric: el.type === 'number' || mode === 'numeric' || mode === 'decimal',
            password: el.type === 'password'
        });
    }, true);
    window.__orcaTouchKeypad = {
        set: function (id, value) {
            var el = target;
            if (id !== serial || !el || !el.isConnected)
                return;
            target = null;
            var proto = el.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
            Object.getOwnPropertyDescriptor(proto, 'value').set.call(el, value);
            el.dispatchEvent(new Event('input', { bubbles: true }));
            el.dispatchEvent(new Event('change', { bubbles: true }));
            if (el.getRootNode().activeElement === el)
                el.blur();
        }
    };
})();
)JS";

static void open_touch_keypad(wxWeakRef<wxWebView> webView, long long serial, const wxString& initial, bool numeric, bool password)
{
    if (!webView)
        return;
    const auto value = Slic3r::GUI::ask_touch_text(webView.get(), initial, numeric, password);
    if (!value || !webView)
        return;
    const std::string literal = nlohmann::json(value->utf8_string()).dump(-1, ' ', true);
    WebView::RunScript(webView.get(), wxString::Format("window.__orcaTouchKeypad && window.__orcaTouchKeypad.set(%lld, %s);",
                                                       serial, wxString::FromUTF8(literal)));
}

void WebView::EnableDelayedTouchKeypads()
{
    while (!g_delay_touch_keypads.empty() && !Slic3r::GUI::wxGetApp().is_adding_script_handler()) {
        auto views = std::move(g_delay_touch_keypads);
        for (const wxWeakRef<wxWebView>& wv : views)
            if (wv)
                EnableTouchKeypad(wv.get());
    }
}

void WebView::EnableTouchKeypad(wxWebView *webView)
{
    if (webView == nullptr || dynamic_cast<FakeWebView *>(webView) != nullptr || !Slic3r::GUI::touch_input_enabled())
        return;
    // AddScriptMessageHandler runs a nested main loop on GTK, so this can be reached while another
    // add is in progress; re-posting with CallAfter would spin inside that loop forever.
    if (Slic3r::GUI::wxGetApp().is_adding_script_handler()) {
        g_delay_touch_keypads.push_back(webView);
        return;
    }
    Slic3r::GUI::wxGetApp().set_adding_script_handler(true);
    const bool added = webView->AddScriptMessageHandler(TOUCH_KEYPAD_HANDLER);
    Slic3r::GUI::wxGetApp().set_adding_script_handler(false);
    EnableDelayedTouchKeypads();
    if (!added) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": could not add the touch keypad script message handler";
        return;
    }

    webView->Bind(wxEVT_WEBVIEW_LOADED, [webView](wxWebViewEvent &evt) {
        WebView::RunScript(webView, TOUCH_KEYPAD_SCRIPT);
        evt.Skip();
    });
    // The GTK backend does not report which handler a message came to, so the payload tells.
    webView->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, [webView](wxWebViewEvent &evt) {
        const nlohmann::json j = nlohmann::json::parse(evt.GetString().utf8_string(), nullptr, false);
        if (!j.is_object() || !j.contains("orca_touch_keypad") || !j["orca_touch_keypad"].is_number_integer()) {
            evt.Skip();
            return;
        }
        auto flag = [&j](const char *key) {
            const auto it = j.find(key);
            return it != j.end() && it->is_boolean() && it->get<bool>();
        };
        const auto      value    = j.find("value");
        const long long serial   = j["orca_touch_keypad"].get<long long>();
        const wxString  initial  = value != j.end() && value->is_string() ? wxString::FromUTF8(value->get<std::string>()) : wxString();
        const bool      numeric  = flag("numeric");
        const bool      password = flag("password");
        wxWeakRef<wxWebView> ref(webView);
        webView->CallAfter([ref, serial, initial, numeric, password] { open_touch_keypad(ref, serial, initial, numeric, password); });
    });
    WebView::RunScript(webView, TOUCH_KEYPAD_SCRIPT);
}

#if wxUSE_WEBVIEW_EDGE
bool WebView::CheckWebViewRuntime()
{
    wxWebViewFactoryEdge factory;
    auto wxVersion = factory.GetVersionInfo(wxVersionContext::RunTime);
    bool present = wxVersion.GetMajor() != 0;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": WebView2 runtime "
                            << (present ? "found, version " : "not found (")
                            << wxVersion.ToString().ToUTF8().data() << (present ? "" : ")");
    return present;
}

bool WebView::DownloadAndInstallWebViewRuntime()
{
    return DownloadAndInstallWV2RT() == 0;
}
#endif
void WebView::LoadUrl(wxWebView * webView, wxString const &url)
{
    auto url2  = url;
#ifdef __WIN32__
    url2.Replace("\\", "/");
#endif
    if (!url2.empty()) { url2 = wxURI(url2).BuildURI(); }
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << url2.ToUTF8();
    webView->LoadURL(url2);
}

bool WebView::RunScript(wxWebView *webView, wxString const &javascript)
{
    if (Slic3r::GUI::wxGetApp().app_config->get("internal_developer_mode") == "true"
            && javascript.find("studio_userlogin") == wxString::npos)
        wxLogMessage("Running JavaScript:\n%s\n", javascript);

    try {
#ifdef __WIN32__
        ICoreWebView2 *   webView2 = (ICoreWebView2 *) webView->GetNativeBackend();
        if (webView2 == nullptr)
            return false;
        return webView2->ExecuteScript(javascript, NULL) == 0;
#elif defined __WXMAC__
        WKWebView * wkWebView = (WKWebView *) webView->GetNativeBackend();
        Slic3r::GUI::WKWebView_evaluateJavaScript(wkWebView, javascript, nullptr);
        return true;
#else
        WebKitWebView *wkWebView = (WebKitWebView *) webView->GetNativeBackend();
        webkit_web_view_run_javascript(
            wkWebView, javascript.utf8_str(), NULL,
            [](GObject *wkWebView, GAsyncResult *res, void *) {
                GError * error = NULL;
                auto result = webkit_web_view_run_javascript_finish((WebKitWebView*)wkWebView, res, &error);
                if (!result)
                    g_error_free (error);
                else
                    webkit_javascript_result_unref (result);
        }, NULL);
        return true;
#endif
    } catch (std::exception &) {
        return false;
    }
}

void WebView::RecreateAll()
{
    auto dark = Slic3r::GUI::wxGetApp().dark_mode();
    wxString language_code = Slic3r::GUI::wxGetApp().current_language_code().BeforeFirst('_');
    language_code          = language_code.ToStdString();
    for (auto webView : g_webviews) {
        webView->SetUserAgent(wxString::Format("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) BBL-Slicer/v%s (%s) BBL-Language/%s",
                                               Slic3r::GUI::wxGetApp().get_bbl_client_version(), dark ? "dark" : "light", language_code.mb_str()));
        // A host-themed WebViewHostDialog re-themes in place (no reload). If it handles
        // the event, skip the reload; legacy pages fall through and reload as before
        // (their own dark.css swap re-themes them on reload).
        wxCommandEvent evt(EVT_WEBVIEW_RECREATED);
        evt.SetEventObject(webView);
        if (!webView->GetEventHandler()->ProcessEvent(evt))
            webView->Reload();
    }
}
