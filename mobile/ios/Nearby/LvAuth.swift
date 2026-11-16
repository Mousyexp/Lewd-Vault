// Google Drive sign-in on iOS, in Apple's sign-in sheet (ASWebAuthenticationSession). Opening Safari
// would send the app to the background, where iOS stops the small local server (src-tauri/src/gdrive.rs)
// that hears Google's answer; the sheet keeps the app in front. Put on the web view next to Nearby's
// "lvBle" by LvNearbyInstaller (LvNearby.swift).
//
// The page (ui/drivesync.js) sends { op: open, url } or { op: close } to webkit.messageHandlers.lvAuth
// and hears window.__lvAuthClosed() when the person closes the sheet.
import AuthenticationServices
import Foundation
import UIKit
import WebKit

final class LvAuth: NSObject, WKScriptMessageHandler, ASWebAuthenticationPresentationContextProviding {
  private weak var webView: WKWebView?
  private var session: ASWebAuthenticationSession?

  init(webView: WKWebView) { self.webView = webView }

  func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
    guard let body = message.body as? [String: Any], let op = body["op"] as? String else { return }
    switch op {
    case "open":
      // (only Google's sign-in page: nothing else gets a sheet)
      guard let s = body["url"] as? String, let url = URL(string: s), url.scheme == "https",
            url.host == "accounts.google.com" else { return }
      open(url)
    case "close":
      let s = session
      session = nil
      s?.cancel()
    default: break
    }
  }

  private func open(_ url: URL) {
    session?.cancel()
    // The answer goes to http://127.0.0.1:<port>, which the sheet loads from the app's own server;
    // no callback scheme is needed, the page closes the sheet once the sign-in is done
    let s = ASWebAuthenticationSession(url: url, callbackURLScheme: nil) { [weak self] _, _ in
      guard let self = self else { return }
      let wasOurs = self.session != nil
      self.session = nil
      if wasOurs { self.webView?.evaluateJavaScript("window.__lvAuthClosed && window.__lvAuthClosed()", completionHandler: nil) }
    }
    s.presentationContextProvider = self
    // (a fresh sign-in: no cookies kept from Safari, none left behind)
    s.prefersEphemeralWebBrowserSession = true
    session = s
    if !s.start() { session = nil; webView?.evaluateJavaScript("window.__lvAuthClosed && window.__lvAuthClosed()", completionHandler: nil) }
  }

  func presentationAnchor(for session: ASWebAuthenticationSession) -> ASPresentationAnchor {
    if let w = webView?.window { return w }
    for scene in UIApplication.shared.connectedScenes {
      if let ws = scene as? UIWindowScene, let w = ws.windows.first(where: { $0.isKeyWindow }) ?? ws.windows.first { return w }
    }
    return ASPresentationAnchor()
  }
}
