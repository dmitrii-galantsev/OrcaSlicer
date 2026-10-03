#ifndef slic3r_GUI_WebView_hpp_
#define slic3r_GUI_WebView_hpp_

#include <wx/string.h>
#include <wx/setup.h>
#include <wx/webview.h>
#include <wx/event.h>

wxDECLARE_EVENT(EVT_WEBVIEW_RECREATED, wxCommandEvent);

class WebView
{
public:
    static wxWebView *CreateWebView(wxWindow *parent, wxString const &url);
#if wxUSE_WEBVIEW_EDGE
    static bool CheckWebViewRuntime();
    static bool DownloadAndInstallWebViewRuntime();
#endif
    static void LoadUrl(wxWebView * webView, wxString const &url);

    static bool RunScript(wxWebView * webView, wxString const & msg);

    // Marks "wx" as registered so CreateWebView's deferred add skips the duplicate.
    static void MarkScriptMessageHandlerAdded(wxWebView * webView);

    // wxWebView::AddScriptMessageHandler() without its wait for the page on GTK.
    static bool AddScriptMessageHandler(wxWebView * webView, const wxString & name);

    // In touch mode, a tap on a text or number input in the page opens the touch keypad and its
    // result is written back as if typed. The page posts through the script message handler
    // named handler, which the owner adds. Call once per webview, after the owner has bound its own
    // wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED handler on it. Does nothing outside touch mode.
    static void EnableTouchKeypad(wxWebView * webView, const wxString & handler);

    static void RecreateAll();
};

#endif // !slic3r_GUI_WebView_hpp_
