import { webview } from '@kit.ArkWeb';
import { BusinessError } from '@kit.BasicServicesKit';

// Only desired per-Web settings live here before attachment. Once attached,
// reads query the actual controller. Cookie values are never cached or logged.
class GuestWebAgent {
  requested: string = '';
  controller?: webview.WebviewController;
}

export class WebPlatformBridge {
  // Android guest pages select their native bridge from the OS token. Keep
  // ArkWeb's real browser version; expose the Android WebView guest contract.
  private defaultGuestAgent(): string {
    return webview.WebviewController.getDefaultUserAgent().replace(/\([^)]*\)/,
      '(Linux; Android 10; HarmonyOS compatibility; wv)');
  }
  private agents: Map<number, GuestWebAgent> = new Map<number, GuestWebAgent>();
  private log: (line: string) => void;
  constructor(log: (line: string) => void) { this.log = log; }

  private agent(web: number): GuestWebAgent {
    if (!Number.isSafeInteger(web) || web <= 0) { throw new Error('Invalid guest Web reference'); }
    let state = this.agents.get(web);
    if (!state) {
      if (this.agents.size >= 128) { throw new Error('Guest Web setting capacity'); }
      state = new GuestWebAgent();
      this.agents.set(web, state);
    }
    return state;
  }

  attach(web: number, controller: webview.WebviewController): void {
    let state = this.agent(web);
    this.applyAgent(controller, state.requested);
    state.controller = controller;
    this.log('WEB_COOKIE op=ua_attach status=1 web=' + web + ' length=' + this.effectiveAgent(state).length);
  }

  private applyAgent(controller: webview.WebviewController, requested: string): void {
    let effective: string = requested.length > 0 ? requested : this.defaultGuestAgent();
    controller.setCustomUserAgent(effective);
    let actual = controller.getCustomUserAgent();
    // Reset is reported by this engine as its effective default UA rather
    // than an empty custom string. Both representations mean the same reset.
    let valid = actual === effective;
    if (!valid) { throw new Error('ArkWeb rejected user agent'); }
    this.log('WEB_COOKIE op=ua_apply default=' + String(requested.length === 0) +
      ' android=' + String(actual.indexOf('Android') >= 0) + ' readback=1');
  }

  detach(web: number, controller: webview.WebviewController): void {
    let state = this.agents.get(web);
    if (state && state.controller === controller) { state.controller = undefined; }
  }

  private effectiveAgent(state: GuestWebAgent): string {
    if (state.controller) {
      let custom = state.controller.getCustomUserAgent();
      return custom.length > 0 ? custom : state.controller.getUserAgent();
    }
    return state.requested.length > 0 ? state.requested : this.defaultGuestAgent();
  }

  private count(value: string): number {
    return value.split(';').filter((part: string): boolean => part.trim().length > 0).length;
  }

  // A reply is sent only after the real ArkWeb operation completes. Errors carry
  // a numeric code only; BusinessError.message can contain Cookie or URL data.
  async request(id: number, op: number, web: number, arg0: string, arg1: string,
    reply: (id: number, status: number, value: string) => void): Promise<void> {
    let value: string = '';
    try {
      switch (op) {
        case 1:
          await webview.WebCookieManager.configCookie(arg0, arg1, false, true);
          value = '1';
          break;
        case 2:
          value = await webview.WebCookieManager.fetchCookie(arg0);
          this.log('WEB_COOKIE op=get status=1 count=' + this.count(value) + ' length=' + value.length);
          break;
        case 3: {
          let existed = webview.WebCookieManager.existCookie();
          await webview.WebCookieManager.clearAllCookies();
          if (webview.WebCookieManager.existCookie()) { throw new Error('Cookie clear verification failed'); }
          value = existed ? '1' : '0';
          break;
        }
        case 4: {
          let before = await webview.WebCookieManager.fetchAllCookies(false);
          let sessions = before.filter((cookie: webview.WebHttpCookie): boolean => cookie.isSessionCookie).length;
          await webview.WebCookieManager.clearSessionCookie();
          let after = await webview.WebCookieManager.fetchAllCookies(false);
          let remaining = after.filter((cookie: webview.WebHttpCookie): boolean => cookie.isSessionCookie).length;
          if (remaining !== 0) { throw new Error('Session Cookie clear verification failed'); }
          this.log('WEB_COOKIE op=remove_session status=1 before_count=' + before.length +
            ' session_count=' + sessions + ' after_count=' + after.length);
          value = sessions > 0 ? '1' : '0';
          break;
        }
        case 5:
          await webview.WebCookieManager.saveCookieAsync();
          value = '1';
          break;
        case 6:
          webview.WebCookieManager.putAcceptCookieEnabled(arg0 === '1');
          value = webview.WebCookieManager.isCookieAllowed() ? '1' : '0';
          if (value !== arg0) { throw new Error('Cookie policy readback failed'); }
          break;
        case 7:
          value = webview.WebCookieManager.isCookieAllowed() ? '1' : '0';
          break;
        case 8:
          value = webview.WebCookieManager.existCookie() ? '1' : '0';
          break;
        case 9:
          value = this.defaultGuestAgent();
          break;
        case 10: {
          let state = this.agent(web);
          if (state.controller) {
            this.applyAgent(state.controller, arg0);
          }
          state.requested = arg0;
          value = '1';
          break;
        }
        case 11:
          value = this.effectiveAgent(this.agent(web));
          break;
        case 12:
          this.agents.delete(web);
          value = '1';
          break;
        case 13: {
          let state = this.agent(web);
          if (!state.controller) { throw new Error('Cookie probe needs an attached Web'); }
          // Read only diagnostics from the live document, never its Cookie
          // values. Neither the page nor the HTTP response is substituted.
          let result = await state.controller.runJavaScript(
            'JSON.stringify({count:document.cookie.split(";").filter(function(s){return s.trim().length>0}).length,' +
            'length:document.cookie.length,diagnosticNames:' +
            'document.cookie.split(";").some(function(s){return s.trim().indexOf("__hmos_diagnostic_session=")===0})&&' +
            'document.cookie.split(";").some(function(s){return s.trim().indexOf("__hmos_diagnostic_persistent=")===0}),' +
            'uaEqual:navigator.userAgent===' + JSON.stringify(this.effectiveAgent(state)) + '})');
          // ArkWeb returns a JSON encoded JS value; a JS string result has one
          // extra JSON layer. Accept either representation, without eval.
          let payload: string = result;
          if (payload.startsWith('"')) { payload = JSON.parse(payload) as string; }
          let decoded = JSON.parse(payload) as PageCookieProbe;
          this.log('WEB_COOKIE_DOCUMENT web=' + web + ' count=' + decoded.count + ' length=' + decoded.length +
            ' diagnosticNames=' + (decoded.diagnosticNames ? 1 : 0) + ' uaEqual=' + (decoded.uaEqual ? 1 : 0));
          value = decoded.diagnosticNames && decoded.uaEqual ? '1' : '0';
          break;
        }
        default:
          this.log('WEB_COOKIE op=' + op + ' status=-2');
          reply(id, -2, ''); return;
      }
      if (value.length > 65535) { throw new Error('Web reply capacity'); }
      if (op !== 2 && op !== 4) {
        this.log('WEB_COOKIE op=' + op + ' status=1 length=' + value.length);
      }
      reply(id, 1, value);
    } catch (error) {
      let code = (error as BusinessError).code ?? -1;
      this.log('WEB_COOKIE op=' + op + ' status=-1 errorCode=' + code);
      reply(id, -1, '');
    }
  }

  // Only metadata from real page state enters diagnostics. These checks do not
  // plant a Cookie or modify the page; explicit VM self-tests do that separately.
  observePage(web: number, controller: webview.WebviewController, url: string): void {
    try {
      let cookie: string = webview.WebCookieManager.fetchCookieSync(url);
      this.log('WEB_COOKIE_PAGE web=' + web + ' count=' + this.count(cookie) + ' length=' + cookie.length);
      let expected = this.effectiveAgent(this.agent(web));
      controller.runJavaScript('navigator.userAgent').then((result: string): void => {
        let actual: string = result;
        try { actual = JSON.parse(result) as string; } catch (ignored) { }
        this.log('WEB_COOKIE_UA_PAGE web=' + web + ' equal=' + (actual === expected ? 1 : 0) +
          ' length=' + actual.length);
      }).catch((error: BusinessError): void => {
        this.log('WEB_COOKIE_UA_PAGE web=' + web + ' status=-1 errorCode=' + error.code);
      });
    } catch (error) {
      this.log('WEB_COOKIE_PAGE web=' + web + ' status=-1 errorCode=' + ((error as BusinessError).code ?? -1));
    }
  }
}

interface PageCookieProbe {
  count: number;
  length: number;
  diagnosticNames: boolean;
  uaEqual: boolean;
}
