import probe from 'libprobe.so';
import { LoadingProgress } from './LoadingProgress';
import { webview } from '@kit.ArkWeb';
import { WebPlatformBridge } from './WebPlatformBridge';
import { common } from '@kit.AbilityKit';
import batteryInfo from '@ohos.batteryInfo';
import commonEventManager from '@ohos.commonEventManager';
import { fileIo as fs } from '@kit.CoreFileKit';
import settings from '@ohos.settings';
import connection from '@ohos.net.connection';
import radio from '@ohos.telephony.radio';
import sim from '@ohos.telephony.sim';
import display from '@ohos.display';
import deviceInfo from '@ohos.deviceInfo';
import window from '@ohos.window';
import hilog from '@ohos.hilog';
import audio from '@ohos.multimedia.audio';
import { BusinessError } from '@kit.BasicServicesKit';
import autoFillManager from '@ohos.app.ability.autoFillManager';
const SETTINGS_DOMAIN_UNSUPPORTED: string = 'unsupported';
const SETTINGS_KEY_UNSUPPORTED: string = '';
const ENABLE_ORIENTATION_SELF_TEST: boolean = false;
const SHOW_LOGIN_TEST_ENTRY: boolean = false;
interface CaptchaJavascriptInterface {
  finishWithResult: (payload: string) => void;
  goBackWithResult: (payload: string) => void;
}
interface HostLoginHistory {
  version: number;
  accounts: string[];
}
interface GenericWebEvaluation {
  id: string;
  revision: number;
  script: string;
}
interface GenericWebSyncPage {
  id: string;
  revision: number;
}
class GenericWebPendingClose {
  id: string;
  action: string;
  revision: number;
  queued: boolean = false;
  absentObserved: boolean = false;
  attempts: number = 0;
  constructor(id: string, action: string, revision: number) {
    this.id = id; this.action = action; this.revision = revision;
  }
}
interface GenericWebInterface {
  name: string;
  methods: string[];
}
class GenericWebJavascriptProxy {
  private webId: string;
  private interfaceName: string;
  private send: (id: string, action: string, payload: string) => number;
  constructor(id: string, name: string, send: (id: string, action: string, payload: string) => number) {
    this.webId = id;
    this.interfaceName = name;
    this.send = send;
  }
  onJSAndDomReady(): void { this.send(this.webId, 'js:' + this.interfaceName + ':onJSAndDomReady', ''); }
  close(payload: string): void { this.send(this.webId, 'js:' + this.interfaceName + ':close', payload); }
  handler(name: string, data: string, callback: string): void {
    this.send(this.webId, 'js:' + this.interfaceName + ':handler', JSON.stringify({ name: name, data: data, callback: callback }));
  }
  finishWithResult(payload: string): void { this.send(this.webId, 'js:' + this.interfaceName + ':finishWithResult', payload); }
  goBackWithResult(payload: string): void { this.send(this.webId, 'js:' + this.interfaceName + ':goBackWithResult', payload); }
  call(payload: string): void { this.send(this.webId, 'js:' + this.interfaceName + ':call', payload); }
  CallbackFromWebView(payload: string): void { this.send(this.webId, 'js:' + this.interfaceName + ':CallbackFromWebView', payload); }
}
class GenericWebBinding {
  name: string;
  methods: string[];
  proxy: GenericWebJavascriptProxy;
  constructor(name: string, methods: string[], proxy: GenericWebJavascriptProxy) {
    this.name = name;
    this.methods = methods;
    this.proxy = proxy;
  }
}
@Observed
class GenericWebPage {
  id: string;
  webRef: number;
  platform: WebPlatformBridge;
  url: string;
  userAgent: string;
  headers: webview.WebHeader[];
  bindings: GenericWebBinding[];
  serial: number;
  nativeRevision: number = 0;
  controller: webview.WebviewController = new webview.WebviewController();
  attached: boolean = false;
  navigationStarted: boolean = false;
  live: boolean = true;
  documentReady: boolean = false;
  scriptRunning: boolean = false;
  pendingScripts: string[] = [];
  onDiagnostic: (phase: string, code: number) => void = (phase: string, code: number): void => {};
  queueScript(script: string): void {
    if (!this.live) { return; }
    if (script.length > 65536 || this.pendingScripts.length >= 16) {
      this.onDiagnostic('evaluate_queue_capacity', -1); return;
    }
    this.pendingScripts.push(script);
    if (!this.attached || !this.documentReady) { this.onDiagnostic('evaluate_deferred', 0); }
    this.flushScripts();
  }
  flushScripts(): void {
    if (!this.live || !this.attached || !this.documentReady || this.scriptRunning || this.pendingScripts.length === 0) { return; }
    let script: string = this.pendingScripts[0];
    this.pendingScripts = this.pendingScripts.slice(1);
    this.scriptRunning = true;
    try {
      this.controller.runJavaScript(script).then(() => {
        this.scriptRunning = false;
        if (this.live) { this.onDiagnostic('evaluate_complete', 0); this.flushScripts(); }
      }).catch((error: BusinessError) => {
        this.scriptRunning = false;
        if (this.live) { this.onDiagnostic('evaluate_failed', error.code); this.flushScripts(); }
      });
    } catch (error) {
      this.scriptRunning = false;
      this.onDiagnostic('evaluate_failed', (error as BusinessError).code);
      this.flushScripts();
    }
  }
  status: string = '正在加载网页…';
  constructor(id: string, url: string, userAgent: string, headers: webview.WebHeader[],
    bindings: GenericWebBinding[], serial: number, webRef: number, platform: WebPlatformBridge) {
    this.id = id;
    this.webRef = webRef;
    this.platform = platform;
    this.url = url;
    this.userAgent = userAgent;
    this.headers = headers;
    this.bindings = bindings;
    this.serial = serial;
  }
}
@Component
struct GenericWebOverlay {
  @ObjectLink page: GenericWebPage;
  @Prop isTop: boolean = true;
  onClose: (id: string, force: boolean) => void = (id: string, force: boolean): void => {};
  onNative: (id: string, action: string, payload: string) => number =
    (id: string, action: string, payload: string): number => { return -1; };
  private loadPage(): void {
    if (!this.page.live || this.page.attached) { return; }
    this.page.attached = true;
    try {
      this.page.platform.attach(this.page.webRef, this.page.controller);
      for (let binding of this.page.bindings) {
        this.page.controller.registerJavaScriptProxy(binding.proxy, binding.name, binding.methods);
      }
    } catch (error) {
      let failure = error as BusinessError;
      this.page.platform.detach(this.page.webRef, this.page.controller);
      this.page.attached = false;
      this.page.status = '网页加载失败，请关闭后重试';
      this.onNative(this.page.id, 'page_error', String(failure.code));
    }
  }
  private navigatePage(): void {
    if (!this.page.live || !this.page.attached || this.page.navigationStarted) { return; }
    this.page.navigationStarted = true;
    try {
      this.page.controller.loadUrl(this.page.url, this.page.headers);
    } catch (error) {
      this.page.status = '网页加载失败，请关闭后重试';
      this.onNative(this.page.id, 'page_error', String((error as BusinessError).code));
    }
  }
  build() {
    Stack({ alignContent: Alignment.Center }) {
      Row() {}.width('100%').height('100%').backgroundColor('#99000000')
        .hitTestBehavior(HitTestMode.Block).onClick(() => {})
      Column() {
        if (this.page.url.indexOf('/agreement/') < 0) {
        Row() {
          Text('网页').fontSize(18).fontColor(Color.Black)
          Blank()
          if (this.page.url.indexOf('/agreement/') < 0) {
            Button('关闭').id('generic-web-close-top').height(32).fontSize(13)
              .onClick(() => { this.onNative(this.page.id, 'path:top_close', ''); this.onClose(this.page.id, false); })
          }
        }.width('100%').padding(8)
        Text(this.page.status).id('generic-web-status').fontSize(12).fontColor(Color.Black)
          .width('100%').padding({ left: 8, right: 8, bottom: 4 })
        }
        Web({ src: 'about:blank', controller: this.page.controller }).id('generic-web-content')
          .javaScriptAccess(true).domStorageAccess(true)
          .onControllerAttached(() => { this.loadPage(); })
          .onPageBegin((event) => {
            if (this.page.live && this.page.navigationStarted &&
              (event.url !== 'about:blank' || this.page.url === 'about:blank')) {
              this.page.documentReady = false;
              this.page.status = '正在加载网页…';
            }
          })
          .onPageEnd((event) => {
            // Finish the component's initial blank navigation before loading the
            // guest URL; otherwise Web(src) can abort the controller navigation.
            if (this.page.live && this.page.attached && !this.page.navigationStarted && event.url === 'about:blank') {
              this.navigatePage(); return;
            }
            if (!this.page.live || !this.page.navigationStarted ||
              (event.url === 'about:blank' && this.page.url !== 'about:blank')) { return; }
            this.page.status = '网页已加载';
            this.page.documentReady = true;
            this.page.flushScripts();
            this.page.platform.observePage(this.page.webRef, this.page.controller, event.url);
            this.onNative(this.page.id, 'page_end', event.url);
            // Observe fixed labels only. Never call acceptance or alter page state.
            this.page.queueScript("(function(){if(window.__pathHomeObserved)return;window.__pathHomeObserved=true;console.info('PATH_HOME:'+((typeof JSShakeBridge!=='undefined')?'bridge_ready':'bridge_missing'));document.addEventListener('click',function(e){var t=e.target;var s=(t.innerText||t.value||'').trim();console.info('PATH_HOME:'+((s==='同意'||s==='接受'||s==='同意并继续'||s==='同意并接受')?'dom_agree':(s==='拒绝'||s==='不同意')?'dom_disagree':'dom_click'));var c=document.querySelector('.scroll-content');if(c)console.info('PATH_HOME:'+((c.scrollTop+c.clientHeight>=c.scrollHeight*.95)?'read_bottom':'read_pending'));},true);})();");
            this.page.queueScript("(function(){console.info('PATH_HOME_PUBLIC:'+location.origin+location.pathname);document.querySelectorAll('script[src]').forEach(function(s){console.info('PATH_HOME_PUBLIC:'+s.src.split(/[?#]/)[0]);});window.addEventListener('error',function(e){console.info('PATH_HOME:'+((e.message||'').indexOf('TypeError')>=0?'type_error':'page_error'));});})();");
          })
          .onTouch((event) => {
            if (event.type === TouchType.Down) { this.onNative(this.page.id, 'path:web_touch', ''); }
          })
          .onConsole((event) => {
            let message: string = event.message.getMessage();
            for (let stage of ['bridge_ready', 'bridge_missing', 'dom_agree', 'dom_disagree', 'dom_click', 'read_bottom', 'read_pending', 'type_error', 'page_error']) {
              if (message === 'PATH_HOME:' + stage) { this.onNative(this.page.id, 'path:' + stage, ''); }
            }
            if (message.startsWith('PATH_HOME_PUBLIC:https://') && message.length < 512 &&
              message.indexOf('?') < 0 && message.indexOf('#') < 0) {
              this.page.onDiagnostic(message, 0);
            }
            return false;
          })
          .onErrorReceive((event) => {
            if (!this.page.live || !this.page.navigationStarted || !event.request.isMainFrame()) { return; }
            this.page.status = '网页加载失败，请关闭后重试';
            this.onNative(this.page.id, 'page_error', String(event.error.getErrorCode()));
          })
          .width('100%').layoutWeight(1)
        if (this.page.url.indexOf('/agreement/') < 0) {
         Row({ space: 12 }) {
          Button('关闭网页').id('generic-web-close-bottom').height(34).fontSize(13)
            .onClick(() => { this.onNative(this.page.id, 'path:bottom_close', ''); this.onClose(this.page.id, false); })
          Button('强制关闭').id('generic-web-force-close').height(34).fontSize(13)
            .onClick(() => { this.onNative(this.page.id, 'path:force_close', ''); this.onClose(this.page.id, true); })
         }.width('100%').justifyContent(FlexAlign.End).padding(8)
        }
      }.id('generic-web-card').width('47%').height('46%')
        .borderRadius(this.page.url.indexOf('/agreement/') >= 0 ? 0 : 12).clip(true)
        .backgroundColor(this.page.url.indexOf('/agreement/') >= 0 ? Color.Transparent : Color.White)
        .hitTestBehavior(HitTestMode.Default).onClick(() => {})
    }.id('generic-web-overlay').width('100%').height('100%')
      .visibility(this.isTop ? Visibility.Visible : Visibility.Hidden)
  }
}
interface LoginPanelMessage {
  process?: number;
  monoMs?: number;
  sdkSavedKnown?: boolean;
  sdkSavedPresent?: boolean;
  sdkSavedName?: string;
  activityClass?: string;
  type?: string;
  webRef?: number;
  id?: string;
  revision?: number;
  script?: string;
  pages?: GenericWebSyncPage[];
  userAgent?: string;
  headers?: webview.WebHeader[];
  interfaces?: GenericWebInterface[];
  sessionId?: string;
  requestId?: string;
  activeRequestId?: string;
  ready?: boolean;
  required?: boolean;
  busy?: boolean;
  phase?: string;
  error?: string;
  url?: string;
  httpStatus?: number;
  code?: string;
  message?: string;
  server_message?: string;
  need_captch?: string;
  interim?: boolean;
  interfaceName?: string;
  timedOut?: boolean;
}
class SettingsKeyMapping {
  domain: number;
  androidKey: string;
  ohosKey: string;
  constructor(domain: number, androidKey: string, ohosKey: string) {
    this.domain = domain;
    this.androidKey = androidKey;
    this.ohosKey = ohosKey;
  }
}
// Confirmed Android -> OHOS key mappings only.  An Android key that is not
// listed here is "unsupported", never "absent".
const SETTINGS_KEY_MAPPINGS: SettingsKeyMapping[] = [
  new SettingsKeyMapping(0, 'airplane_mode_on', settings.general.AIRPLANE_MODE_STATUS),
  new SettingsKeyMapping(1, 'accelerometer_rotation', settings.general.ACCELEROMETER_ROTATION_STATUS),
  new SettingsKeyMapping(1, 'screen_brightness', settings.display.SCREEN_BRIGHTNESS_STATUS),
  new SettingsKeyMapping(1, 'font_scale', settings.display.FONT_SCALE),
  new SettingsKeyMapping(1, 'screen_off_timeout', settings.display.SCREEN_OFF_TIMEOUT)
];
const SETTINGS_CAPABILITY_SUMMARY: string = 'Settings capability evidence: supported domains Global(0)->deviceShared and System(1)->userProperty; ' +
  'confirmed key map: Global.airplane_mode_on->general.AIRPLANE_MODE_STATUS, System.accelerometer_rotation->general.ACCELEROMETER_ROTATION_STATUS, ' +
  'System.screen_brightness->display.SCREEN_BRIGHTNESS_STATUS, System.font_scale->display.FONT_SCALE, System.screen_off_timeout->display.SCREEN_OFF_TIMEOUT; ' +
  'unsupported: Secure(2) (no public USER_SECURE domain) and every unlisted key -> reply -1 (backend unsupported, fail-closed), never 0 absent. ' +
  'The C probe key __native_probe_missing_key__ is deliberately unlisted, so it now returns -1; 0 is reserved for a mapped key proven absent. ' +
  'Display/network snapshots come from separate real sources; this is not a claim that all bridges are verified.';
@Entry @Component struct Index {
  @State loginVisible: boolean = false;
  @State sdkSavedKnown: boolean = false;
  @State titleProcess: number = -1;
  @State titleInitRequired: boolean = false;
  @State sdkSavedPresent: boolean = false;
  @State sdkSavedName: string = '';
  @State loginInputRequested: boolean = false;
  @State savedLoginAccounts: string[] = [];
  @State accountListVisible: boolean = false;
  @State loginHistoryStatus: string = '';
  private returningLoginAttempted: boolean = false;
  private restoreSavedLoginPending: boolean = false;
  @State loginAccount: string = '';
  @State loginPassword: string = '';
  @State loginBusy: boolean = false;
  @State loginReady: boolean = false;
  @State loginPhase: string = '运行环境未就绪';
  @State loginHasResponse: boolean = false;
  @State diagnosticsVisible: boolean = false;
  @StorageLink('pageGeometryStatus') pageGeometryStatus: string = 'HOST_PAGESIZE=pending\nGUEST_PAGE_ALIGN=pending';
  @StorageLink('vmBootStatus') @Watch('updateSplash') vmBootStatus: string = 'vm_last_step=waiting';
  @StorageLink('vmBootBlocking') vmBootBlocking: boolean = false;
  @StorageLink('loadingPrepared') @Watch('updateSplash') loadingPrepared: boolean = false;
  @StorageLink('loadingSnapshot') @Watch('updateSplash') loadingSnapshot: string = 'stage=0 submissions=0 mono_ms=0';
  @StorageLink('loadingError') loadingError: string = '';
  @StorageLink('loadingSplashDisabled') loadingSplashDisabled: boolean = false;
  @State splashVisible: boolean = true;
  @State splashOpacity: number = 1;
  @State splashProgress: number = 0;
  @State splashLabel: string = '检查安装包';
  @State splashLogs: boolean = false;
  private splashModel: LoadingProgress = new LoadingProgress();
  private splashFading: boolean = false;
  private splashFadeTimer: number = -1;
  private updateSplash(): void {
    this.splashModel.update(this.vmBootStatus, this.loadingPrepared, this.loadingSnapshot);
    this.splashProgress = this.splashModel.value;
    this.splashLabel = this.splashModel.label;
    if (this.splashModel.presented && !this.splashFading) {
      this.splashFading = true;
      this.probeLog('SPLASH_FIRST_SUBMISSION ' + this.loadingSnapshot + ' wall=' + Date.now());
      this.getUIContext().animateTo({ duration: 200, curve: Curve.Linear }, () => { this.splashOpacity = 0; });
      this.splashFadeTimer = setTimeout(() => { this.splashVisible = false; this.splashFadeTimer = -1;
        this.probeLog('SPLASH_FADE_COMPLETE wall=' + Date.now()); }, 200);
    }
  }
  private retrySplash(): void {
    try {
      // UIAbilityContext.restartApp is phone-only (801 on tablets).
      this.context().getApplicationContext().restartApp({ bundleName: 'top.weearc.bangdream.nativeprobe', abilityName: 'EntryAbility' });
    } catch (error) {
      let detail = error as BusinessError;
      this.probeLog('SPLASH_RETRY_FAILED code=' + detail.code);
      this.loadingError = '重启失败，请关闭应用后重试。';
    }
  }
  @State loginResult: string = '';
  @State passwordVaultBusy: boolean = false;
  @State passwordVaultFillBusy: boolean = false;
  @State passwordVaultStatus: string = '';
  private passwordVaultAutoRequestId: string = '';
  @State genericWebPages: GenericWebPage[] = [];
  private genericWebSerial: number = 0;
  private genericWebSyncNeeded: boolean = true;
  private genericWebRetryTimer: number = -1;
  private genericWebPendingCloses: GenericWebPendingClose[] = [];
  private genericWebDismissed: GenericWebSyncPage[] = [];
  private genericWebPendingEvaluations: GenericWebEvaluation[] = [];
  @State captchaUrl: string = '';
  @State captchaVisible: boolean = false;
  @State captchaNeeded: boolean = false;
  private captchaInterface: string = 'BiliJsObject';
  private captchaActive: boolean = false;
  private captchaController: webview.WebviewController = new webview.WebviewController();
  private captchaWebRef: number = 0;
  private captchaNavigationStarted: boolean = false;
  private webPlatform: WebPlatformBridge = new WebPlatformBridge((line: string): void => { this.probeLog(line); });
  private captchaProxy: CaptchaJavascriptInterface = {
    finishWithResult: (payload: string): void => { this.deliverCaptchaResult(payload, 'finishWithResult'); },
    goBackWithResult: (payload: string): void => { this.deliverCaptchaResult(payload, 'goBackWithResult'); }
  };
  private loginRequestId: string = '';
  private loginTimer: number = -1;
  private loginRefreshTimer: number = -1;
  private loginWorkerBusy: boolean = false;
  private gameAutoStarted: boolean = false;
  @State result: string = 'Ready';
  @State status: string = 'Waiting for surface';
  @State sdkBusy: boolean = false;
  @State guestMounted: boolean = false;
  @State guestVertical: boolean = true;
  @State unityPresentation: boolean = true;
  @State touchGridBusy: boolean = false;
  @State hostInfo: string = 'Host bridge not initialized';
  private operatorNumeric: string = '';
  private operatorNameCache: string = '';
  private telephonyPrefetchOk: boolean = false;
  private hostErrors: string = '';
  private unitySurfaceFocused: boolean = false;
  private hostSystemUiFlags: number = 0;
  private batterySubscriber: commonEventManager.CommonEventSubscriber | undefined = undefined;
  private batterySequence: number = 0;
  private batterySnapshots: number = 0;
  private batteryEvents: number = 0;
  private batteryPublish(sequence: number, accepted: boolean, delivered: boolean): void {
    if (!accepted) { probe.batteryUpdate(sequence, false, delivered, 0, 1, 1, 0, 0, 0, 0, ''); return; }
    let charge: number = batteryInfo.chargingStatus;
    let status: number = charge === 1 ? 2 : (charge === 2 ? 4 : (charge === 3 ? 5 : 1));
    let healthMap: number[] = [1, 2, 3, 5, 7, 4];
    let pluggedMap: number[] = [0, 1, 2, 4];
    let health: number = batteryInfo.healthStatus;
    let plugged: number = batteryInfo.pluggedType;
    if (charge === 2 && plugged === 0) { status = 3; }
    probe.batteryUpdate(sequence, true, delivered, batteryInfo.batterySOC, status,
      healthMap[health] ?? 1, pluggedMap[plugged] ?? 0, batteryInfo.batteryTemperature,
      Math.trunc(batteryInfo.voltage / 1000), batteryInfo.isBatteryPresent ? 1 : 0, batteryInfo.technology);
  }
  private async batteryStart(sequence: number): Promise<void> {
    this.batterySequence = sequence;
    try {
      let sub = await commonEventManager.createSubscriber({ events: [commonEventManager.Support.COMMON_EVENT_BATTERY_CHANGED] });
      if (this.batterySequence !== sequence) { return; }
      this.batterySubscriber = sub;
      commonEventManager.subscribe(sub, (error, data) => {
        if (this.batterySequence !== sequence) { return; }
        if (error) { probe.batteryUpdate(sequence, false, true, 0, 1, 1, 0, 0, 0, 0, ''); this.status = 'Battery event error: ' + String(error.code); this.batteryStop(sequence); return; }
        try { this.batteryPublish(sequence, true, true); this.batterySnapshots++; this.batteryEvents++; this.status = 'Battery snapshots: ' + this.batterySnapshots + ', OS events: ' + this.batteryEvents; }
        catch (e) { this.status = 'Battery snapshot failed: ' + String(e); }
      });
      this.batteryPublish(sequence, true, false);
      this.batterySnapshots++;
      this.status = 'Battery registration call accepted; snapshots: ' + this.batterySnapshots + ', OS events: ' + this.batteryEvents;
    } catch (e) { this.batteryPublish(sequence, false, false); this.batteryStop(sequence); this.status = 'Battery registration failed: ' + String(e); }
  }
  private batteryStop(sequence: number): void {
    if (this.batterySequence !== sequence) { return; }
    this.batterySequence = 0;
    let sub = this.batterySubscriber; this.batterySubscriber = undefined;
    if (sub) { try { commonEventManager.unsubscribe(sub); } catch (e) { this.status = 'Battery unsubscribe failed: ' + String(e); } }
  }
  /* Host-side orientation: the guest only asked for a layout direction; the
   * HarmonyOS window itself must also be turned. Results go to hilog and the
   * shared engine-probe.txt channel so a failed API call is visible instead of
   * silently leaving the tablet in portrait. */
  private probeLog(line: string): void {
    try { hilog.info(0x0000, 'NativeProbe', '%{public}s', line); } catch (e) { }
    // hilog is system bounded. Do not synchronously append from the UI thread.
  }
  private writeDiagnosticReport(path: string, report: string): void {
    // Explicit diagnostics action only; never called from frame/touch callbacks.
    let large: boolean = false;
    try { large = fs.readTextSync(this.context().filesDir + '/sdkdex/diagnostic-log-large').trim() === '1'; } catch (e) { }
    let limit: number = large ? 4194304 : 262144; // <= 3 UTF-8 bytes per UTF-16 code unit.
    let backup: string = path + '.1';
    if (fs.accessSync(backup)) { fs.unlinkSync(backup); }
    if (fs.accessSync(path)) {
      if (fs.statSync(path).size <= (large ? 16777216 : 1048576)) { fs.renameSync(path, backup); }
      else { fs.unlinkSync(path); }
    }
    let output = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
    try { fs.writeSync(output.fd, report.length > limit ? report.slice(0, limit) + '\n[diagnostic report capped]\n' : report); }
    finally { fs.closeSync(output); }
  }
  private async applyWindowOrientation(vertical: boolean): Promise<void> {
    let target: window.Orientation = vertical ? window.Orientation.PORTRAIT : window.Orientation.LANDSCAPE;
    let label: string = vertical ? 'PORTRAIT' : 'LANDSCAPE';
    let mainWindow: window.Window;
    try {
      mainWindow = await window.getLastWindow(this.context());
    } catch (e) {
      let err = e as BusinessError;
      this.probeLog('HOST_ORIENTATION ok=0 stage=getLastWindow target=' + label +
        ' code=' + String(err.code) + ' msg=' + String(err.message) + ' fallback=layout_only');
      return;
    }
    try {
      await mainWindow.setPreferredOrientation(target);
      this.probeLog('HOST_ORIENTATION ok=1 api=setPreferredOrientation target=' + label);
      return;
    } catch (e) {
      let err = e as BusinessError;
      this.probeLog('HOST_ORIENTATION ok=0 api=setPreferredOrientation target=' + label +
        ' code=' + String(err.code) + ' msg=' + String(err.message));
    }
    /* Newer API reports an explicit execution result; use it only as a retry,
     * never as a silent success. */
    try {
      let result: window.OrientationResult = await mainWindow.setPreferredOrientationWithResult(target);
      this.probeLog('HOST_ORIENTATION ok=1 api=setPreferredOrientationWithResult target=' + label +
        ' result=' + String(result));
      return;
    } catch (e) {
      let err = e as BusinessError;
      this.probeLog('HOST_ORIENTATION ok=0 api=setPreferredOrientationWithResult target=' + label +
        ' code=' + String(err.code) + ' msg=' + String(err.message) + ' fallback=layout_only');
    }
  }
  /* One-shot startup probe: prove the real window API works on this device,
   * then restore portrait. This is diagnostic-only and never drives guest UI. */
  private async probeOrientationSelfTest(): Promise<void> {
    let mainWindow: window.Window;
    try {
      mainWindow = await window.getLastWindow(this.context());
    } catch (e) {
      let err = e as BusinessError;
      this.probeLog('HOST_ORIENTATION_SELFTEST ok=0 stage=getLastWindow code=' + String(err.code));
      return;
    }
    try {
      await mainWindow.setPreferredOrientation(window.Orientation.LANDSCAPE);
      this.probeLog('HOST_ORIENTATION_SELFTEST ok=1 api=setPreferredOrientation target=LANDSCAPE');
    } catch (e) {
      let err = e as BusinessError;
      this.probeLog('HOST_ORIENTATION_SELFTEST ok=0 api=setPreferredOrientation code=' +
        String(err.code) + ' msg=' + String(err.message));
      return;
    }
    setTimeout(() => {
      mainWindow.setPreferredOrientation(window.Orientation.PORTRAIT).then(() => {
        this.probeLog('HOST_ORIENTATION_SELFTEST restored=PORTRAIT');
      }).catch((e: BusinessError) => {
        this.probeLog('HOST_ORIENTATION_SELFTEST restore_failed code=' + String(e.code));
      });
    }, 1500);
  }
  private receiveNativeEvent(message: string): void {
    if (message.indexOf('{') === 0) {
      try {
        let event: LoginPanelMessage = JSON.parse(message) as LoginPanelMessage;
        if (event.type === 'game/progress') {
          this.titleProcess = event.process ?? -1;
          if (this.titleProcess === 0 || this.titleProcess >= 15) { this.titleInitRequired = false; }
          this.probeLog('TAP_PROGRESS_RECEIVED process=' + String(this.titleProcess) +
            ' native_mono_ms=' + String(event.monoMs ?? 0) + ' host_epoch_ms=' + String(Date.now()));
          return;
        }
        if (event.type === 'game/init-required') {
          this.titleInitRequired = true;
          this.probeLog('TAP_INIT_REQUIRED code=3004 source=original_sdk_onFailed_return');
          return;
        }
        if (event.type === 'webview/open') { this.openGenericWeb(event); return; }
        if (event.type === 'webview/evaluate') { this.evaluateGenericWeb(event); return; }
        if (event.type === 'webview/synced') {
          let complete: boolean = true;
          for (let expected of event.pages ?? []) {
            if (this.isGenericWebDismissed(expected.id)) { continue; }
            let found: boolean = false;
            for (let page of this.genericWebPages) {
              if (page.live && page.id === expected.id && page.nativeRevision >= expected.revision) { found = true; break; }
            }
            if (!found) { complete = false; }
          }
          let retained: GenericWebPendingClose[] = [];
          for (let pending of this.genericWebPendingCloses) {
            let present: boolean = false;
            for (let expected of event.pages ?? []) { if (expected.id === pending.id) { present = true; break; } }
            // Ack has no request sequence. An earlier snapshot may arrive
            // after close intent, so absence is diagnostic, not a close ack.
            if (pending.queued && !present && !pending.absentObserved) {
              pending.absentObserved = true;
              this.probeLog('GENERIC_WEB id=' + pending.id + ' revision=' + String(pending.revision) +
                ' phase=close_snapshot_absent awaiting=closed');
            }
            retained.push(pending);
          }
          this.genericWebPendingCloses = retained;
          this.genericWebSyncNeeded = !complete || retained.length > 0;
          return;
        }
        if (event.type === 'webview/closed') {
          if (event.id) {
            let revision: number = this.genericWebRevision(event.id);
            this.rememberGenericWebDismissal(event.id, revision);
            this.probeLog('GENERIC_WEB id=' + event.id + ' revision=' + String(revision) +
              ' phase=close_ack source=closed guest_result=see_native_log');
            this.removeGenericWeb(event.id);
            this.genericWebPendingCloses = this.genericWebPendingCloses.filter((pending: GenericWebPendingClose): boolean => pending.id !== event.id);
          }
          return;
        }
        if (event.type === 'login/required') {
          this.titleInitRequired = false;
          this.probeLog('LOGIN_PANEL_REQUIRED activity=' + (event.activityClass ?? 'unknown'));
          this.openLoginPanel();
          return;
        }
        if (event.type === 'login/completed' && event.requestId === this.loginRequestId) {
          this.rememberSuccessfulLogin();
          this.loginPassword = '';
          this.loginVisible = false;
          this.loginBusy = false;
          this.clearLoginTimer();
          this.probeLog('HOST_LOGIN final_sdk_success=1 waiting_home=1');
          return;
        }
        if (event.type === 'login/captcha') {
          if (event.requestId !== this.loginRequestId || !event.url || event.url.indexOf('https://') !== 0) { return; }
          let alreadyVisible = this.captchaVisible;
          if (alreadyVisible) { this.webPlatform.detach(this.captchaWebRef, this.captchaController); }
          this.captchaUrl = event.url;
          this.captchaWebRef = event.webRef ?? 0;
          this.captchaNavigationStarted = false;
          this.captchaActive = true;
          this.captchaInterface = event.interfaceName ?? 'BiliJsObject';
          this.captchaNeeded = true;
          this.captchaVisible = true;
          if (alreadyVisible) {
            try {
              this.webPlatform.attach(this.captchaWebRef, this.captchaController);
              this.captchaNavigationStarted = true;
              this.captchaController.loadUrl(this.captchaUrl);
            } catch (error) { this.probeLog('WEB_COOKIE captcha_reattach status=-1'); }
          }
          this.probeLog('CAPTCHA_WEB_RECEIVED urlLength=' + String(event.url.length));
          return;
        }
        if (event.type === 'login/state') {
          if (event.requestId && event.requestId !== this.loginRequestId) { return; }
          this.applyLoginState(event);
          return;
        }
        if (event.type === 'login/response') {
          if (event.requestId !== this.loginRequestId || event.interim !== true) { return; }
          this.clearLoginTimer();
          this.loginHasResponse = true;
          this.loginBusy = false;
          this.loginPhase = '';
          this.loginResult = this.loginResponseText(event);
          this.captchaNeeded = event.need_captch === '1' || event.code === '200007';
          // Ask the system while the input nodes and their values still exist.
          // This is a save request, not a claim of final SDK login success.
          if (event.code === '0' && event.need_captch !== '1' && this.loginRequestId.length > 0 &&
            this.loginRequestId !== this.passwordVaultAutoRequestId) {
            this.passwordVaultAutoRequestId = this.loginRequestId;
            this.requestPasswordVaultSave();
          }
          // Only the whitelisted native event is retained; never input values.
          this.probeLog('LOGIN_PANEL_RESPONSE ' + message);
          return;
        }
      } catch (ignored) { }
    }
    if (message.indexOf('BATTERY_START:') === 0) { this.batteryStart(Number(message.split(':')[1])); return; }
    if (message.indexOf('BATTERY_STOP:') === 0) { this.batteryStop(Number(message.split(':')[1])); return; }
    if (message.indexOf('GUEST_MOUNT:') !== 0) { this.status = message; return; }
    let parts: string[] = message.split(':');
    let sequence: number = Number(parts[1]);
    let orientation: number = Number(parts[2]);
    // The old diagnostic LinearLayout mount removes the XComponent. It is
    // unsupported while Unity owns the screen; fail closed and keep its surface.
    let accepted: boolean = parts.length === 3 && (orientation === 0 || orientation === 1) && !this.unityPresentation;
    if (!accepted) { this.probeLog('GUEST_MOUNT rejected=1 unity_surface_preserved=1'); }
    if (accepted) {
      this.guestVertical = orientation === 1;
      this.guestMounted = true;
      this.applyWindowOrientation(this.guestVertical).then(() => {}).catch((e: BusinessError) => {
        this.probeLog('HOST_ORIENTATION ok=0 stage=dispatch code=' + String(e.code) + ' msg=' + String(e.message));
      });
    }
    probe.ackGuestView(sequence, accepted);
  }
  private genericWebCommand(id: string, action: string, payload: string): number {
    let live: boolean = false;
    for (let page of this.genericWebPages) { if (page.id === id && page.live) { live = true; break; } }
    if (!live && action !== 'close' && action !== 'force_close' && action !== 'sync') { return -1; }
    // Interface/method identifiers only; never log the bridge payload.
    let phase: string = action;
    try {
      let result: number = probe.webviewCommand(id, action, payload);
      this.probeLog('GENERIC_WEB id=' + id + ' phase=' + phase + ' accepted=' + String(result === 0));
      return result;
    } catch (error) {
      let failure = error as BusinessError;
      this.probeLog('GENERIC_WEB id=' + id + ' phase=' + phase + ' errorCode=' + String(failure.code));
      return -4;
    }
  }
  private retryGenericWebCommands(): void {
    let retained: GenericWebPendingClose[] = [];
    for (let pending of this.genericWebPendingCloses) {
      pending.attempts += 1;
      let result: number = pending.queued ? 0 : this.genericWebCommand(pending.id, pending.action, '');
      if (!pending.queued && result === 0) {
        pending.queued = true;
        this.probeLog('GENERIC_WEB id=' + pending.id + ' revision=' + String(pending.revision) +
          ' phase=close_queued action=' + pending.action + ' guest_result=pending');
      }
      if (result === -1) {
        this.probeLog('GENERIC_WEB id=' + pending.id + ' revision=' + String(pending.revision) +
          ' phase=close_ack source=registry_absent guest_result=unknown');
        continue;
      }
      if ((result === 0 || result === -3 || result === -4) && pending.attempts < 20) { retained.push(pending); }
      else { this.probeLog('GENERIC_WEB id=' + pending.id + ' revision=' + String(pending.revision) +
        ' phase=close_unconfirmed queued=' + String(pending.queued) + ' attempts=' + String(pending.attempts)); }
    }
    this.genericWebPendingCloses = retained;
    if (retained.length > 0) { this.genericWebSyncNeeded = true; }
    if (this.genericWebSyncNeeded) { this.genericWebCommand('', 'sync', ''); }
  }
  private isGenericWebDismissed(id: string): boolean {
    for (let dismissed of this.genericWebDismissed) { if (dismissed.id === id) { return true; } }
    return false;
  }
  private genericWebRevision(id: string): number {
    for (let page of this.genericWebPages) { if (page.id === id) { return page.nativeRevision; } }
    for (let pending of this.genericWebPendingCloses) { if (pending.id === id) { return pending.revision; } }
    for (let dismissed of this.genericWebDismissed) { if (dismissed.id === id) { return dismissed.revision; } }
    return 0;
  }
  private rememberGenericWebDismissal(id: string, revision: number): void {
    if (this.isGenericWebDismissed(id)) { return; }
    if (this.genericWebDismissed.length >= 64) { this.genericWebDismissed.shift(); }
    // Native reopening creates a new id. Keep this id's tombstone after ack
    // so a delayed snapshot cannot recreate the dismissed Web component.
    this.genericWebDismissed.push({ id: id, revision: revision });
  }
  private genericWebGenerationCommand(id: string, generation: number, action: string, payload: string): number {
    for (let page of this.genericWebPages) {
      if (page.id === id && page.serial === generation && page.live) {
        if (page.attached && page.navigationStarted && action.endsWith(':onJSAndDomReady')) {
          page.documentReady = true;
          page.flushScripts();
        }
        return this.genericWebCommand(id, action, payload);
      }
    }
    return -1;
  }
  private evaluateGenericWeb(event: LoginPanelMessage): void {
    let id: string = event.id ?? '';
    if (!/^[A-Za-z0-9_.:-]{1,128}$/.test(id) || event.script === undefined || this.isGenericWebDismissed(id)) { return; }
    for (let page of this.genericWebPages) {
      if (page.id !== id || !page.live) { continue; }
      if (page.nativeRevision < (event.revision ?? 0)) { break; }
      if (page.nativeRevision !== (event.revision ?? 0)) {
        this.probeLog('GENERIC_WEB id=' + id + ' phase=evaluate_stale'); return;
      }
      page.queueScript(event.script); return;
    }
    if (event.script.length > 65536 || this.genericWebPendingEvaluations.length >= 32) {
      this.probeLog('GENERIC_WEB id=' + id + ' phase=evaluate_pending_capacity'); return;
    }
    this.genericWebPendingEvaluations.push({ id: id, revision: event.revision ?? 0, script: event.script });
    this.genericWebSyncNeeded = true;
    this.probeLog('GENERIC_WEB id=' + id + ' phase=evaluate_pending_open');
  }
  private openGenericWeb(event: LoginPanelMessage): void {
    let id: string = event.id ?? '';
    let url: string = event.url ?? '';
    if (!/^[A-Za-z0-9_.:-]{1,128}$/.test(id) || url.length === 0) { return; }
    if (this.isGenericWebDismissed(id)) {
      this.probeLog('GENERIC_WEB id=' + id + ' revision=' + String(event.revision ?? 0) +
        ' phase=open_suppressed reason=dismissed dismissed_revision=' + String(this.genericWebRevision(id)));
      return;
    }
    let existing: number = -1;
    for (let index: number = 0; index < this.genericWebPages.length; index++) {
      if (this.genericWebPages[index].id === id) { existing = index; break; }
    }
    if (existing !== -1 && this.genericWebPages[existing].nativeRevision >= (event.revision ?? 0)) {
      if (this.genericWebPages[existing].nativeRevision > (event.revision ?? 0)) {
        this.probeLog('GENERIC_WEB id=' + id + ' revision=' + String(event.revision ?? 0) + ' phase=open_suppressed reason=stale_revision');
      }
      return;
    }
    if (existing === -1 && this.genericWebPages.length >= 16) {
      this.probeLog('GENERIC_WEB id=' + id + ' phase=stack_limit');
      this.closeGenericWeb(id, true);
      return;
    }
    let generation: number = this.genericWebSerial + 1;
    let bindings: GenericWebBinding[] = [];
    let interfaces: GenericWebInterface[] = event.interfaces ?? [];
    let supported: string[] = ['onJSAndDomReady', 'close', 'handler', 'finishWithResult',
      'goBackWithResult', 'call', 'CallbackFromWebView'];
    for (let item of interfaces) {
      if (!/^[A-Za-z_$][A-Za-z0-9_$]{0,127}$/.test(item.name)) { continue; }
      let methods: string[] = [];
      for (let method of item.methods) {
        if (supported.indexOf(method) !== -1 && methods.indexOf(method) === -1) { methods.push(method); }
      }
      if (methods.length === 0) { continue; }
      let proxy: GenericWebJavascriptProxy = new GenericWebJavascriptProxy(id, item.name,
        (webId: string, action: string, payload: string): number => { return this.genericWebGenerationCommand(webId, generation, action, payload); });
      bindings.push(new GenericWebBinding(item.name, methods, proxy));
    }
    if (existing !== -1) { this.removeGenericWeb(id, true); }
    this.genericWebSerial = generation;
    let page: GenericWebPage = new GenericWebPage(id, url, event.userAgent ?? '', event.headers ?? [],
      bindings, this.genericWebSerial, event.webRef ?? 0, this.webPlatform);
    page.nativeRevision = event.revision ?? 0;
    page.onDiagnostic = (phase: string, code: number): void => {
      this.probeLog('GENERIC_WEB id=' + id + ' phase=' + phase + ' errorCode=' + String(code));
    };
    let pending: GenericWebEvaluation[] = [];
    for (let evaluation of this.genericWebPendingEvaluations) {
      if (evaluation.id !== id) { pending.push(evaluation); }
      else if (evaluation.revision === page.nativeRevision) { page.queueScript(evaluation.script); }
      else { this.probeLog('GENERIC_WEB id=' + id + ' phase=evaluate_stale'); }
    }
    this.genericWebPendingEvaluations = pending;
    let pages: GenericWebPage[] = this.genericWebPages.slice();
    pages.push(page);
    this.genericWebPages = pages;
    this.probeLog('GENERIC_WEB id=' + id + ' revision=' + String(page.nativeRevision) + ' phase=open urlLength=' + String(url.length));
  }
  private removeGenericWeb(id: string, preservePending: boolean = false): void {
    if (!/^[A-Za-z0-9_.:-]{1,128}$/.test(id)) { return; }
    if (!preservePending) { this.genericWebPendingEvaluations = this.genericWebPendingEvaluations.filter((evaluation: GenericWebEvaluation): boolean => evaluation.id !== id); }
    let pages: GenericWebPage[] = [];
    for (let page of this.genericWebPages) {
      if (page.id !== id) { pages.push(page); continue; }
      page.live = false;
      page.documentReady = false;
      page.pendingScripts = [];
      if (page.attached) {
        this.webPlatform.detach(page.webRef, page.controller);
        try { page.controller.stop(); } catch (ignored) { }
        for (let binding of page.bindings) {
          try { page.controller.deleteJavaScriptRegister(binding.name); } catch (ignored) { }
        }
      }
    }
    this.genericWebPages = pages;
    this.probeLog('GENERIC_WEB id=' + id + ' phase=closed');
  }
  private closeGenericWeb(id: string, force: boolean): void {
    let revision: number = this.genericWebRevision(id);
    this.probeLog('GENERIC_WEB id=' + id + ' revision=' + String(revision) +
      ' phase=close_intent action=' + (force ? 'force_close' : 'close') + ' visible_pages=' + String(this.genericWebPages.length));
    // Local dismissal is unconditional, including a failed native command.
    this.removeGenericWeb(id);
    this.rememberGenericWebDismissal(id, revision);
    let found: boolean = false;
    for (let pending of this.genericWebPendingCloses) {
      if (pending.id === id) {
        if (force && pending.action !== 'force_close') { pending.action = 'force_close'; pending.queued = false; pending.attempts = 0; }
        found = true; break;
      }
    }
    if (!found && this.genericWebPendingCloses.length < 32) {
      this.genericWebPendingCloses.push(new GenericWebPendingClose(id, force ? 'force_close' : 'close', revision));
    }
    this.retryGenericWebCommands();
  }
  onBackPress(): boolean {
    if (this.genericWebPages.length > 0) {
      this.genericWebCommand(this.genericWebPages[this.genericWebPages.length - 1].id, 'path:system_back', '');
      this.closeGenericWeb(this.genericWebPages[this.genericWebPages.length - 1].id, false);
      return true;
    }
    if (this.captchaVisible) {
      if (this.captchaActive) { this.deliverCaptchaResult('', 'goBackWithResult'); }
      else { this.webPlatform.detach(this.captchaWebRef, this.captchaController); this.captchaVisible = false; }
      return true;
    }
    if (this.loginVisible) { this.loginVisible = false; this.loginPassword = ''; return true; }
    return false;
  }
  private async runTouchGrid(): Promise<void> {
    if (this.touchGridBusy || !this.unityPresentation) { return; }
    this.touchGridBusy = true;
    let run: string = String(Date.now());
    let completed: number = 0;
    try {
      let mainWindow = await window.getLastWindow(this.context());
      let area = mainWindow.getWindowProperties().windowRect;
      for (let cell: number = 0; cell < 48; cell++) {
        let x: number = area.width * ((cell % 8) + 0.5) / 8;
        let y: number = area.height * (Math.floor(cell / 8) + 0.5) / 6;
        this.probeLog('TOUCH_GRID run=' + run + ' cell=' + cell + ' x=' + x + ' y=' + y + ' window=' + area.width + 'x' + area.height);
        probe.injectTap(x, y);
        await new Promise<void>((resolve) => { setTimeout(resolve, 350); });
        let path: string = this.context().filesDir + '/sdkdex/touch-grid-state.txt';
        let file = fs.openSync(path, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
        fs.writeSync(file.fd, run + '|' + cell + '|' + x + '|' + y);
        fs.closeSync(file);
        let ack: string = '';
        for (let retry: number = 0; retry < 100; retry++) {
          try { ack = fs.readTextSync(this.context().filesDir + '/sdkdex/touch-grid-ack.txt').trim(); } catch (e) { ack = ''; }
          if (ack === run + '|' + cell || ack === 'STOP|' + run + '|' + cell) { break; }
          await new Promise<void>((resolve) => { setTimeout(resolve, 50); });
        }
        if (ack === 'STOP|' + run + '|' + cell) { this.probeLog('TOUCH_GRID stopped_after_capture cell=' + cell); break; }
        if (ack !== run + '|' + cell) { this.probeLog('TOUCH_GRID capture_ack_timeout cell=' + cell); break; }
        completed++;
      }
    } catch (e) { this.probeLog('TOUCH_GRID error=' + String(e)); }
    this.probeLog('TOUCH_GRID_COMPLETE run=' + run + ' captured=' + completed);
    this.touchGridBusy = false;
  }
  private context(): common.UIAbilityContext { return getContext(this) as common.UIAbilityContext; }
  private clearLoginTimer(): void {
    if (this.loginTimer !== -1) { clearTimeout(this.loginTimer); this.loginTimer = -1; }
  }
  private phaseLabel(phase: string): string {
    switch (phase) {
      case 'queued':
      case 'entry':
      case 'cipher':
      case 'RSA':
      case 'login': return '正在登录，请稍候';
      case 'callback': return '正在确认登录结果，请稍候';
      case 'ready': return '运行环境已就绪';
      case 'preparing': return '运行环境未就绪';
      case 'waiting_captcha': return '需要完成验证码验证';
      case 'response': return '登录请求已提交，请稍候';
      case 'timeout': return '网络请求超时，请重试';
      case 'failed': return '登录失败，请重试';
      default: return '运行环境未就绪';
    }
  }
  private deliverCaptchaResult(payload: string, method: string): void {
    if (!this.captchaActive || payload.length > 8192 ||
      (method !== 'finishWithResult' && method !== 'goBackWithResult')) { return; }
    let status: number = probe.loginCaptchaResult(this.loginRequestId, payload, method);
    this.probeLog('CAPTCHA_RESULT dispatch=' + String(status) + ' payloadLength=' + String(payload.length));
    if (status === 0) {
      this.captchaActive = false;
      this.webPlatform.detach(this.captchaWebRef, this.captchaController);
      this.captchaVisible = false;
      this.captchaUrl = '';
      this.captchaNeeded = false;
      this.loginResult = method === 'goBackWithResult' ? '验证已取消' : '验证中…';
    }
    else { this.loginResult = '验证结果提交失败，请重试'; }
  }
  private loginResponseText(event: LoginPanelMessage): string {
    if (event.need_captch === '1' || event.code === '200007') { return '需要完成验证码验证'; }
    let message: string = ((event.server_message ?? '') + ' ' + (event.message ?? '')).toLowerCase();
    if (message.indexOf('账号或密码错误') >= 0 || message.indexOf('用户名或密码错误') >= 0 ||
      message.indexOf('incorrect password') >= 0 || message.indexOf('wrong password') >= 0 ||
      message.indexOf('password error') >= 0 || message.indexOf('account or password error') >= 0 ||
      message.indexOf('invalid username or password') >= 0) { return '账号或密码错误'; }
    // Network observation is interim; code=0 alone is not SDK login success.
    if (event.code === '0') { return event.interim === false ? '登录成功' : '登录请求已提交，请稍候'; }
    let code: string = event.code ?? '';
    if (/^\d+$/.test(code)) { return '登录失败（错误码 ' + code + '）'; }
    return '登录失败，请重试';
  }
  private applyLoginState(state: LoginPanelMessage): void {
    this.sdkSavedKnown = state.sdkSavedKnown === true;
    this.sdkSavedPresent = state.sdkSavedPresent === true;
    this.sdkSavedName = state.sdkSavedName ?? '';
    this.loginReady = state.ready === true;
    this.loginWorkerBusy = state.busy === true;
    this.loginBusy = this.loginWorkerBusy && !this.loginHasResponse && state.timedOut !== true && state.phase !== 'callback';
    if (!this.loginHasResponse) {
      this.loginPhase = this.phaseLabel(state.phase ?? 'unavailable');
      if (state.timedOut) {
        this.loginResult = '网络请求超时，请重试';
        this.clearLoginTimer();
      } else if (state.error && (state.phase === 'failed' || state.phase === 'stopped')) {
        this.loginResult = '登录失败，请重试';
      }
    }
    if (state.phase === 'callback' || state.phase === 'failed' || state.phase === 'stopped') { this.clearLoginTimer(); }
  }
  private refreshLoginState(): void {
    try {
      let state: LoginPanelMessage = JSON.parse(probe.loginGetState()) as LoginPanelMessage;
      this.applyLoginState(state);
    } catch (ignored) { this.loginReady = false; this.loginPhase = '运行环境未就绪'; }
  }
  private openLoginPanel(): void {
    this.loginInputRequested = false;
    try {
      let state: LoginPanelMessage = JSON.parse(probe.loginGetState()) as LoginPanelMessage;
      this.loginRequestId = state.activeRequestId ?? '';
      this.applyLoginState(state);
    } catch (ignored) { this.loginResult = '运行环境未就绪'; }
    this.loginVisible = true;
    this.probeLog('LOGIN_PANEL_VISIBLE arkts=1 waiting_user=1');
    this.diagnosticsVisible = false;
  }
  private loadLoginHistory(): void {
    try {
      let history: HostLoginHistory = JSON.parse(fs.readTextSync(this.context().filesDir + '/host-login-history.json')) as HostLoginHistory;
      if (history.version !== 1 || !Array.isArray(history.accounts)) { throw new Error('invalid history'); }
      this.savedLoginAccounts = history.accounts.filter((name: string): boolean => typeof name === 'string' && name.length > 0 && name.length <= 512).slice(0, 8);
      this.loginHistoryStatus = this.savedLoginAccounts.length > 0 ? '可选择已保存账号，或切换账号' : '首次登录，请输入账号密码';
      this.probeLog('HOST_LOGIN first=' + String(this.savedLoginAccounts.length === 0) + ' source=own_storage');
    } catch (ignored) {
      this.savedLoginAccounts = [];
      this.loginHistoryStatus = '未找到可用登录记录，请输入账号密码';
      this.probeLog('HOST_LOGIN first=true source=own_storage unavailable_or_empty=1');
    }
  }
  private rememberSuccessfulLogin(): void {
    if (!this.loginAccount) { return; }
    let names: string[] = [this.loginAccount].concat(this.savedLoginAccounts.filter((name: string): boolean => name !== this.loginAccount)).slice(0, 8);
    let history: HostLoginHistory = { version: 1, accounts: names };
    try {
      let file = fs.openSync(this.context().filesDir + '/host-login-history.json', fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      try { fs.writeSync(file.fd, JSON.stringify(history)); } finally { fs.closeSync(file); }
      this.savedLoginAccounts = names;
    } catch (ignored) { this.probeLog('HOST_LOGIN history_write_failed=1'); }
  }
  private switchLoginAccount(): void {
    if (this.loginBusy || this.loginWorkerBusy) { return; }
    this.restoreSavedLoginPending = false;
    this.openLoginPanel();
    this.loginInputRequested = true;
    this.loginPassword = '';
    this.loginAccount = '';
    this.accountListVisible = false;
    this.loginHistoryStatus = '选择已保存账号，或输入其他账号';
    this.probeLog('HOST_LOGIN switch_account=1 arkts=1');
  }
  private submitSavedLogin(): void {
    if (!this.sdkSavedPresent || !this.loginReady || this.loginWorkerBusy) { return; }
    this.loginRequestId = Date.now().toString();
    this.loginHasResponse = false;
    this.loginResult = '';
    this.loginPassword = '';
    let rc: number = probe.loginSavedLogin(this.loginRequestId);
    this.loginBusy = rc === 0;
    this.loginResult = rc === 0 ? '正在登录已有账号' : '无法恢复已保存账号，请选择更换账号';
    this.refreshLoginState();
  }
  private requestPasswordVaultSave(): void {
    if (this.passwordVaultBusy || this.passwordVaultFillBusy || !this.loginVisible || !this.loginAccount || !this.loginPassword) { return; }
    this.passwordVaultBusy = true;
    this.passwordVaultStatus = '请根据系统提示确认保存';
    try {
      autoFillManager.requestAutoSave(this.getUIContext(), {
        onSuccess: (): void => {
          this.passwordVaultBusy = false;
          // API11 confirms handling of the request, not a readable vault entry.
          this.passwordVaultStatus = '系统已处理保存请求，请以系统提示为准';
          this.probeLog('PASSWORD_VAULT_SAVE handled=true');
        },
        onFailure: (): void => {
          this.passwordVaultBusy = false;
          this.passwordVaultStatus = '系统未新增保存，请尝试从密码保险箱填充';
          this.probeLog('PASSWORD_VAULT_SAVE handled=false');
        }
      });
    } catch (error) {
      this.passwordVaultBusy = false;
      this.passwordVaultStatus = '未能发起保存，请检查系统密码保险箱设置后重试';
      let code: number = (error as BusinessError).code;
      this.probeLog('PASSWORD_VAULT_SAVE handled=false code=' + String(code));
    }
  }
  private passwordVaultNode(id: string, type: autoFillManager.AutoFillType,
    value: string, focused: boolean, pageLeft: number, pageTop: number): autoFillManager.PageNodeInfo {
    let ui = this.getUIContext();
    let node = ui.getFrameNodeById(id);
    if (!node) { throw new Error('Password vault input node unavailable'); }
    let bounds = ui.getComponentUtils().getRectangleById(id);
    return {
      id: node.getUniqueId(), autoFillType: type, value: value, isFocus: focused,
      rect: { left: bounds.windowOffset.x - pageLeft, top: bounds.windowOffset.y - pageTop,
        width: bounds.size.width, height: bounds.size.height }
    };
  }
  private requestPasswordVaultFill(): void {
    if (!this.loginVisible || this.passwordVaultBusy || this.passwordVaultFillBusy) { return; }
    // API12 retains the component-driven native autofill path. The explicit
    // API26 request is available only on devices that actually implement it.
    if (deviceInfo.sdkApiVersion < 26 || typeof autoFillManager.requestAutoFill !== 'function') {
      this.restoreSavedLoginPending = false;
      this.passwordVaultStatus = '请点击账号框使用系统填充，或在密码保险箱中选择本应用记录';
      this.probeLog('HOST_LOGIN vault_restore=unsupported fallback=manual');
      return;
    }
    this.passwordVaultFillBusy = true;
    this.passwordVaultStatus = '请在系统密码保险箱中选择账号';
    try {
      let ui = this.getUIContext();
      let page = ui.getComponentUtils().getRectangleById('login-host-page');
      let accountNode: autoFillManager.PageNodeInfo = this.passwordVaultNode('login-account',
        autoFillManager.AutoFillType.USER_NAME, this.loginAccount, false, page.windowOffset.x, page.windowOffset.y);
      let passwordNode: autoFillManager.PageNodeInfo = this.passwordVaultNode('login-password',
        autoFillManager.AutoFillType.PASSWORD, this.loginPassword, true, page.windowOffset.x, page.windowOffset.y);
      let data: autoFillManager.ViewData = {
        bundleName: this.context().abilityInfo.bundleName,
        pageUrl: ui.getRouter().getState().path,
        pageNodeInfos: [accountNode, passwordNode],
        pageRect: { left: page.windowOffset.x, top: page.windowOffset.y,
          width: page.size.width, height: page.size.height }
      };
      let request: autoFillManager.FillRequest = {
        type: autoFillManager.AutoFillType.PASSWORD, viewData: data,
        triggerType: autoFillManager.AutoFillTriggerType.MANUAL_REQUEST
      };
      autoFillManager.requestAutoFill(ui, request, {
        onSuccess: (filled: autoFillManager.ViewData): void => {
          this.passwordVaultFillBusy = false;
          let applied: boolean = false;
          let matchedAccount: boolean = false;
          let matchedPassword: boolean = false;
          if (this.loginVisible) {
            for (let field of filled.pageNodeInfos) {
              if (field.id === accountNode.id && field.autoFillType === autoFillManager.AutoFillType.USER_NAME) {
                this.loginAccount = field.value; matchedAccount = true;
              } else if (field.id === passwordNode.id && field.autoFillType === autoFillManager.AutoFillType.PASSWORD) {
                this.loginPassword = field.value; matchedPassword = true;
              }
            }
            // Some providers return their own node IDs. Only the two requested
            // credential types may supply a fallback, once per field.
            for (let field of filled.pageNodeInfos) {
              if (!matchedAccount && field.autoFillType === autoFillManager.AutoFillType.USER_NAME) {
                this.loginAccount = field.value; matchedAccount = true;
              } else if (!matchedPassword && field.autoFillType === autoFillManager.AutoFillType.PASSWORD) {
                this.loginPassword = field.value; matchedPassword = true;
              }
            }
            applied = matchedAccount || matchedPassword;
            this.passwordVaultStatus = applied ? '已从系统填入，请确认后登录' : '系统未返回可填充内容，请检查本应用的密码保险箱记录';
          }
          this.probeLog('PASSWORD_VAULT_FILL handled=true applied=' + String(applied) +
            ' matched_account_node=' + String(matchedAccount) + ' matched_password_node=' + String(matchedPassword) +
            ' account_applied=' + String(matchedAccount) + ' password_applied=' + String(matchedPassword) +
            ' account_nonempty=' + String(this.loginAccount.length > 0) + ' password_nonempty=' + String(this.loginPassword.length > 0));
          let restore: boolean = this.restoreSavedLoginPending;
          this.restoreSavedLoginPending = false;
          if (restore && this.loginVisible && matchedAccount && matchedPassword && this.loginAccount && this.loginPassword) {
            this.refreshLoginState();
            if (this.loginReady) {
              this.probeLog('HOST_LOGIN vault_restore=filled auto_login_attempt=1');
              this.submitLogin();
            } else { this.probeLog('HOST_LOGIN vault_restore=filled backend_ready=0 fallback=manual'); }
          }
        },
        onFailure: (failure: autoFillManager.FillFailureResult): void => {
          this.restoreSavedLoginPending = false;
          this.passwordVaultFillBusy = false;
          this.passwordVaultStatus = '未完成填充，请检查密码保险箱中的本应用记录后重试';
          this.probeLog('PASSWORD_VAULT_FILL handled=false errCode=' + String(failure.errCode));
        }
      });
    } catch (error) {
      this.passwordVaultFillBusy = false;
      this.restoreSavedLoginPending = false;
      this.passwordVaultStatus = '未能发起填充，请检查系统密码保险箱设置后重试';
      this.probeLog('PASSWORD_VAULT_FILL handled=false errCode=' + String((error as BusinessError).code));
    }
  }
  private submitLogin(): void {
    this.refreshLoginState();
    if (!this.loginReady || !this.loginAccount || !this.loginPassword || this.loginBusy) { return; }
    if (this.loginWorkerBusy) {
      // A visible HTTP result ends this UI submission, not the SDK captcha flow.
      this.loginPhase = this.loginHasResponse ? '请先完成本次验证码，再重新登录' : '上一请求仍在处理，请稍后重试';
      return;
    }
    this.loginHasResponse = false;
    this.loginRequestId = String(Date.now());
    let rc: number = probe.loginSubmit(this.loginRequestId, this.loginAccount, this.loginPassword);
    this.loginBusy = rc === 0;
    this.loginResult = rc === 0 ? '' : '提交未接受：运行环境未就绪、正在处理或输入过长';
    this.loginPhase = rc === 0 ? '正在登录，请稍候' : '运行环境未就绪';
    this.clearLoginTimer();
    if (rc === 0) {
      let submittedId: string = this.loginRequestId;
      this.loginTimer = setTimeout(() => {
        this.loginTimer = -1;
        if (submittedId !== this.loginRequestId || this.loginHasResponse) { return; }
        // Native monotonic deadline also records the last phase/exception.
        this.refreshLoginState();
      }, 15100);
    }
  }
  private registerHostBridge(): void {
    try {
      // ArkWeb's initialization API must run on the ArkTS main thread.
      webview.WebviewController.initializeWebEngine();
      probe.registerHostHandler((message: string) => { this.handleHostRpc(message); });
      this.hostInfo = 'Host handler registered: settings (4 mapped keys; Secure/unmapped fail closed) / network / display';
    } catch (error) {
      this.hostInfo = 'Host handler registration failed: ' + String(error);
    }
  }
  private async startGameOnce(): Promise<void> {
    if (this.gameAutoStarted) { return; }
    this.gameAutoStarted = true;
    await this.refreshTelephony();
    await this.refreshDisplaySnapshot();
    probe.appVmWindowReady();
  }
  private async initializeHostSnapshots(): Promise<void> {
    await this.refreshTelephony();
    await this.refreshDisplaySnapshot();
  }
  private handleHostRpc(message: string): void {
    let id: number = 0;
    try {
      let parts: string[] = message.split('|');
      let operation: string = parts[0];
      id = Number(parts[1]);
      if (operation === 'web_platform') {
        this.webPlatform.request(id, Number(parts[2]), Number(parts[3]),
          decodeURIComponent(parts[4]), decodeURIComponent(parts[5]),
          (replyId: number, status: number, value: string): void => { probe.hostReply(replyId, status, value); });
        return;
      }
      if (operation === 'settings_get') {
        this.settingsGet(id, Number(parts[2]), decodeURIComponent(parts[3]));
        return;
      }
      if (operation === 'settings_put') {
        this.settingsPut(id, Number(parts[2]), decodeURIComponent(parts[3]), decodeURIComponent(parts[4]));
        return;
      }
      if (operation === 'settings_delete') {
        // @ohos.settings has no delete API.  Never turn a delete into an
        // empty-string write; report failure so the guest sees false.
        probe.hostReply(id, -1, '');
        return;
      }
      if (operation === 'network') {
        this.replyNetwork(id);
        return;
      }
      if (operation === 'window_feature') {
        this.requestWindowFeature(id, Number(parts[2]));
        return;
      }
      if (operation === 'drawn_complete') {
        this.context().reportDrawnCompleted((error) => {
          let code: number = error ? error.code : 0;
          this.probeLog('HOST_DRAWN_COMPLETE code=' + code + ' guest_report_only=1 visible_frame_verified=0');
          probe.hostReply(id, code === 0 ? 0 : -1, '');
        });
        return;
      }
      if (operation === 'system_ui') {
        this.requestSystemUi(id, Number(parts[2]));
        return;
      }
      if (operation === 'display_rotation') {
        let rotation: number = display.getDefaultDisplaySync().rotation;
        // Display rotation is a quarter-turn enum; retain all four values.
        // Touch coordinates remain in the existing full-screen surface space.
        if (rotation < 0 || rotation > 3 || Math.floor(rotation) !== rotation) { probe.hostReply(id, -1, ''); return; }
        console.info('DISPLAY_ROTATION oh=' + rotation + ' android=' + rotation);
        probe.hostReply(id, 1, String(rotation));
        return;
      }
      if (operation === 'unity_mount') { this.confirmUnitySurface(id); return; }
      if (operation === 'unity_focus') { this.focusUnitySurface(id); return; }
      if (operation === 'unity_state') { this.replyUnityState(id); return; }
      if (operation === 'window_flags') { this.applyWindowFlags(id, Number(parts[2]), Number(parts[3])); return; }
      if (operation === 'audio_volume') { this.replyAudioVolume(id, Number(parts[2])); return; }
      probe.hostReply(id, -1, '');
    } catch (error) {
      this.hostErrors = 'host RPC failed: ' + String(error);
      if (id > 0) { try { probe.hostReply(id, -1, ''); } catch (ignored) { } }
    }
  }
  private async confirmUnitySurface(id: number): Promise<void> {
    try {
      let mainWindow = await window.getLastWindow(this.context());
      // Keep the same XComponent node; guestMounted selects a different,
      // diagnostic layout and must never be used for Unity presentation.
      this.unityPresentation = true;
      await mainWindow.setPreferredOrientation(window.Orientation.LANDSCAPE);
      this.probeLog('HOST_ORIENTATION ok=1 target=LANDSCAPE source=unity_mount');
      await mainWindow.setWindowLayoutFullScreen(true);
      await mainWindow.setWindowSystemBarEnable([]);
      for (let attempt: number = 0; attempt < 40; attempt++) {
        await new Promise<void>((resolve) => { setTimeout(resolve, 25); });
        let bounds = this.getUIContext().getComponentUtils().getRectangleById('unity-host-surface');
        let area = mainWindow.getWindowProperties().windowRect;
        if (area.width > area.height && bounds.size.width > bounds.size.height &&
          Math.abs(bounds.size.width - area.width) < 4 && Math.abs(bounds.size.height - area.height) < 4) { break; }
      }
      await this.refreshDisplaySnapshot();
      let rect = this.getUIContext().getComponentUtils().getRectangleById('unity-host-surface');
      let area = mainWindow.getWindowProperties().windowRect;
      let visible: boolean = !this.guestMounted && mainWindow.isWindowShowing() &&
        rect.size.width > rect.size.height && area.width > area.height &&
        Math.abs(rect.size.width - area.width) < 4 && Math.abs(rect.size.height - area.height) < 4;
      this.probeLog('UNITY_HOST_MOUNT visible=' + visible + ' bounds=' + rect.size.width + 'x' + rect.size.height +
        ' window=' + area.width + 'x' + area.height + ' fullscreen_requested=1 node_preserved=1');
      probe.hostReply(id, visible ? 1 : -1, '');
    } catch (error) { this.probeLog('UNITY_HOST_MOUNT error=' + String(error)); probe.hostReply(id, -1, ''); }
  }
  private async focusUnitySurface(id: number): Promise<void> {
    try {
      this.getUIContext().getFocusController().requestFocus('unity-host-surface');
      for (let attempt: number = 0; attempt < 20 && !this.unitySurfaceFocused; attempt++) {
        await new Promise<void>((resolve) => { setTimeout(resolve, 25); });
      }
      console.info('UNITY_VIEW_FOCUS observed_onFocus=' + this.unitySurfaceFocused);
      probe.hostReply(id, this.unitySurfaceFocused ? 1 : 0, '');
    } catch (error) { probe.hostReply(id, -1, ''); }
  }
  private async replyUnityState(id: number): Promise<void> {
    try {
      let mainWindow = await window.getLastWindow(this.context());
      let showing: boolean = mainWindow.isWindowShowing();
      let focused: boolean = mainWindow.isFocused();
      console.info('UNITY_ACTIVITY_STATE showing=' + showing + ' window_focused=' + focused);
      probe.hostReply(id, 1, String((showing ? 1 : 0) | (focused ? 2 : 0)));
    } catch (error) { probe.hostReply(id, -1, ''); }
  }
  private async applyWindowFlags(id: number, flags: number, mask: number): Promise<void> {
    if ((mask & ~1024) !== 0 || (flags & ~1024) !== 0) { probe.hostReply(id, -1, ''); return; }
    try {
      let mainWindow = await window.getLastWindow(this.context());
      let bars: Array<'status' | 'navigation'> = [];
      if ((flags & 1024) === 0) { bars.push('status'); }
      if ((this.hostSystemUiFlags & 2) === 0) { bars.push('navigation'); }
      await mainWindow.setWindowSystemBarEnable(bars);
      this.hostSystemUiFlags = (this.hostSystemUiFlags & ~4) | ((flags & 1024) !== 0 ? 4 : 0);
      console.info('WINDOW_FLAGS flags=' + flags + ' mask=' + mask + ' host_request_ack=1 visibility_verified=0');
      probe.hostReply(id, 0, '');
    } catch (error) { probe.hostReply(id, -1, ''); }
  }
  private async replyAudioVolume(id: number, streamType: number): Promise<void> {
    try {
      // Only Android stream types with a same-meaning OHOS AudioVolumeType are
      // mapped.  STREAM_SYSTEM/NOTIFICATION/BLUETOOTH_SCO have no confirmed
      // equivalent here, so they fail closed instead of reusing another
      // stream's volume.
      let volumeType: audio.AudioVolumeType = audio.AudioVolumeType.MEDIA;
      let supported: boolean = true;
      if (streamType === 0) { volumeType = audio.AudioVolumeType.VOICE_CALL; }
      else if (streamType === 2) { volumeType = audio.AudioVolumeType.RINGTONE; }
      else if (streamType === 3) { volumeType = audio.AudioVolumeType.MEDIA; }
      else if (streamType === 4) { volumeType = audio.AudioVolumeType.ALARM; }
      else { supported = false; }
      if (!supported) { probe.hostReply(id, -1, ''); return; }
      let volume: number = await audio.getAudioManager().getVolume(volumeType);
      console.info('AUDIO_VOLUME stream=' + streamType + ' ohos_type=' + volumeType + ' volume=' + volume);
      probe.hostReply(id, 1, String(volume));
    } catch (error) {
      probe.hostReply(id, -1, '');
    }
  }
  private async requestSystemUi(id: number, flags: number): Promise<void> {
    if ((flags & ~(1 | 2 | 4 | 256 | 512 | 1024)) !== 0 || ((flags & 1) !== 0 && (flags & 6) !== 6)) { probe.hostReply(id, -1, ''); return; }
    try {
      let mainWindow = await window.getLastWindow(this.context());
      let immersiveLayout: boolean = (flags & (4 | 512 | 1024)) !== 0;
      await mainWindow.setWindowLayoutFullScreen(immersiveLayout);
      let bars: Array<'status' | 'navigation'> = [];
      if ((flags & 4) === 0) { bars.push('status'); }
      if ((flags & 2) === 0) { bars.push('navigation'); }
      await mainWindow.setWindowSystemBarEnable(bars);
      let actual: boolean = mainWindow.getWindowProperties().isLayoutFullScreen;
      this.hostSystemUiFlags = flags;
      console.info('SYSTEM_UI flags=' + flags + ' layout_actual=' + actual + ' bars_request_ack=1 visibility_verified=0');
      probe.hostReply(id, actual === immersiveLayout ? 0 : -1, '');
    } catch (error) {
      this.recordHostIssue('System UI request failed: ' + String(error));
      probe.hostReply(id, -1, '');
    }
  }
  private async requestWindowFeature(id: number, feature: number): Promise<void> {
    if (feature !== 1) { probe.hostReply(id, -1, ''); return; }
    try {
      let mainWindow = await window.getLastWindow(this.context());
      mainWindow.setWindowDecorVisible(false);
      let visible: boolean = mainWindow.getWindowDecorVisible();
      probe.hostReply(id, visible ? -1 : 0, '');
      this.status = 'Window title request: applied; visible=' + visible;
    } catch (error) {
      let windowError = error as BusinessError;
      this.recordHostIssue('Window title request failed: code=' + windowError.code + '; ' + windowError.message);
      // 801 explicitly means unsupported, not success. Java receives false.
      // Permission/window failures remain errors. A successful setter alone
      // never proves that the requested decoration state took effect.
      probe.hostReply(id, windowError.code === 801 ? 1 : -1, '');
    }
  }
  private recordHostIssue(message: string): void {
    let combined: string = this.hostErrors.length > 0 ? this.hostErrors + '; ' + message : message;
    this.hostErrors = combined.length > 512 ? combined.substring(combined.length - 512) : combined;
  }
  private settingsDomainName(domain: number): string {
    if (domain === 0) { return settings.domainName.DEVICE_SHARED; }
    if (domain === 1) { return settings.domainName.USER_PROPERTY; }
    // The public @ohos.settings API exposes no USER_SECURE domain constant.
    // Return an explicit unsupported signal; the value must never be passed to
    // getValueSync/setValueSync and must not be reported as absent.
    return SETTINGS_DOMAIN_UNSUPPORTED;
  }
  private settingsDomainSupported(domain: number): boolean {
    return domain === 0 || domain === 1;
  }
  private settingsDomainReason(domain: number): string {
    if (domain === 2) { return 'Secure(caller domain 2) has no public @ohos.settings USER_SECURE domain'; }
    return 'caller domain ' + domain + ' has no mapped @ohos.settings domain';
  }
  private ohosSettingKey(domain: number, name: string): string {
    for (let index: number = 0; index < SETTINGS_KEY_MAPPINGS.length; index++) {
      let mapping: SettingsKeyMapping = SETTINGS_KEY_MAPPINGS[index];
      if (mapping.domain === domain && mapping.androidKey === name) { return mapping.ohosKey; }
    }
    // Never synthesize settings.general.<name>/settings.display.<name>: an
    // unmapped key means this bridge cannot answer, not that the key is absent.
    return SETTINGS_KEY_UNSUPPORTED;
  }
  private settingsKeyReason(domain: number, name: string): string {
    let label: string = domain === 0 ? 'Global' : (domain === 1 ? 'System' : ('domain ' + domain));
    return label + '.' + name + ' is not in the confirmed Android->OHOS mapping table';
  }
  private settingsGet(id: number, domain: number, name: string): void {
    let domainName: string = this.settingsDomainName(domain);
    if (!this.settingsDomainSupported(domain) || domainName === SETTINGS_DOMAIN_UNSUPPORTED) {
      this.recordHostIssue('settings_get unsupported domain: ' + this.settingsDomainReason(domain) + '; replied -1 (backend unsupported)');
      probe.hostReply(id, -1, '');
      return;
    }
    let ohosKey: string = this.ohosSettingKey(domain, name);
    if (ohosKey === SETTINGS_KEY_UNSUPPORTED) {
      this.recordHostIssue('settings_get key mapping missing: ' + this.settingsKeyReason(domain, name) + '; replied -1 (backend unsupported)');
      probe.hostReply(id, -1, '');
      return;
    }
    let sentinel: string = '\u0001NATIVE_PROBE_ABSENT\u0002';
    try {
      let value: string = settings.getValueSync(this.context(), ohosKey, sentinel, domainName);
      if (value === sentinel) { probe.hostReply(id, 0, ''); return; }
      if (value.length >= 4096) { probe.hostReply(id, -1, ''); return; }
      probe.hostReply(id, 1, value);
    } catch (error) {
      let code: number = (error as BusinessError).code;
      probe.hostReply(id, (code === 201 || code === 401) ? -2 : -1, '');
    }
  }
  private settingsPut(id: number, domain: number, name: string, value: string): void {
    let domainName: string = this.settingsDomainName(domain);
    if (!this.settingsDomainSupported(domain) || domainName === SETTINGS_DOMAIN_UNSUPPORTED) {
      this.recordHostIssue('settings_put unsupported domain: ' + this.settingsDomainReason(domain) + '; replied -1 (backend unsupported)');
      probe.hostReply(id, -1, '');
      return;
    }
    let ohosKey: string = this.ohosSettingKey(domain, name);
    if (ohosKey === SETTINGS_KEY_UNSUPPORTED) {
      this.recordHostIssue('settings_put key mapping missing: ' + this.settingsKeyReason(domain, name) + '; replied -1 (backend unsupported)');
      probe.hostReply(id, -1, '');
      return;
    }
    try {
      let accepted: boolean = settings.setValueSync(this.context(), ohosKey, value, domainName);
      probe.hostReply(id, accepted ? 0 : -1, '');
    } catch (error) {
      let code: number = (error as BusinessError).code;
      probe.hostReply(id, (code === 201 || code === 401) ? -2 : -1, '');
    }
  }
  private mapRadioTech(tech: number): number {
    // OHOS RadioTechnology -> Android TelephonyManager.NETWORK_TYPE_*.
    if (tech === radio.RadioTechnology.RADIO_TECHNOLOGY_GSM) { return 16; }
    if (tech === radio.RadioTechnology.RADIO_TECHNOLOGY_1XRTT) { return 7; }
    if (tech === radio.RadioTechnology.RADIO_TECHNOLOGY_WCDMA) { return 3; }
    if (tech === radio.RadioTechnology.RADIO_TECHNOLOGY_HSPA) { return 10; }
    if (tech === radio.RadioTechnology.RADIO_TECHNOLOGY_HSPAP) { return 15; }
    if (tech === radio.RadioTechnology.RADIO_TECHNOLOGY_TD_SCDMA) { return 17; }
    if (tech === radio.RadioTechnology.RADIO_TECHNOLOGY_EVDO) { return 5; }
    if (tech === radio.RadioTechnology.RADIO_TECHNOLOGY_EHRPD) { return 14; }
    if (tech === radio.RadioTechnology.RADIO_TECHNOLOGY_LTE) { return 13; }
    if (tech === radio.RadioTechnology.RADIO_TECHNOLOGY_LTE_CA) { return 19; }
    if (tech === radio.RadioTechnology.RADIO_TECHNOLOGY_IWLAN) { return 18; }
    if (tech === radio.RadioTechnology.RADIO_TECHNOLOGY_NR) { return 20; }
    if (tech === radio.RadioTechnology.RADIO_TECHNOLOGY_UNKNOWN) { return 0; }
    return -1;
  }
  private mapSimState(state: number): number[] {
    // [TelephonyManager.SIM_STATE_*, valid].  LOCKED cannot distinguish
    // PIN/PUK/network lock, so it stays invalid instead of guessing.
    if (state === sim.SimState.SIM_STATE_UNKNOWN) { return [0, 1]; }
    if (state === sim.SimState.SIM_STATE_NOT_PRESENT) { return [1, 1]; }
    if (state === sim.SimState.SIM_STATE_LOCKED) { return [0, 0]; }
    if (state === sim.SimState.SIM_STATE_NOT_READY) { return [6, 1]; }
    if (state === sim.SimState.SIM_STATE_READY) { return [5, 1]; }
    if (state === sim.SimState.SIM_STATE_LOADED) { return [10, 1]; }
    return [0, 0];
  }
  private replyNetwork(id: number): void {
    let cv: number = 0, hd: number = 0, co: number = 0, av: number = 0, me: number = 0;
    let ty: number = 0, tv: number = 0, tr: number = 0, ca: number = 0, nh: number = 0;
    let telv: number = 0, nt: number = 0, ntv: number = 0, ss: number = 0, ssv: number = 0;
    let pt: number = 0, ptv: number = 0, onv: number = 0, onav: number = 0;
    let operatorNumeric: string = '';
    let operatorName: string = '';
    try {
      let has: boolean = connection.hasDefaultNetSync();
      cv = 1;
      hd = has ? 1 : 0;
      if (has) {
        let handle = connection.getDefaultNetSync();
        nh = handle.netId;
        let capabilitiesInfo = connection.getNetCapabilitiesSync(handle);
        let transports: number = 0;
        for (let bearer of capabilitiesInfo.bearerTypes) {
          let value: number = Number(bearer);
          if (value >= 0 && value <= 31) { transports = transports | (1 << value); }
        }
        let capabilityBits: number = 0;
        let capabilityList: connection.NetCap[] | undefined = capabilitiesInfo.networkCap;
        if (capabilityList) {
          for (let capability of capabilityList) {
            let value: number = Number(capability);
            // Android NET_CAPABILITY_* bits 0..30 are stable.  OHOS
            // CHECKING_CONNECTIVITY=31 collides with Android MMTEL=31, so it
            // is deliberately not forwarded as an Android capability bit.
            if (value >= 0 && value <= 30) { capabilityBits = capabilityBits | (1 << value); }
          }
          me = capabilityList.indexOf(connection.NetCap.NET_CAPABILITY_NOT_METERED) >= 0 ? 0 : 1;
        } else {
          me = 1;
        }
        tr = transports >>> 0;
        ca = capabilityBits >>> 0;
        let primary: number = capabilitiesInfo.bearerTypes.length > 0 ? Number(capabilitiesInfo.bearerTypes[0]) : -1;
        let typeMap: number[] = [0, 1, 7, 9, 17]; // cellular, Wi-Fi, Bluetooth, Ethernet, VPN
        if (primary >= 0 && primary < typeMap.length) { ty = typeMap[primary]; tv = 1; }
        co = 1;
        av = 1;
      }
    } catch (error) {
      cv = 0; hd = 0; co = 0; av = 0; me = 0; ty = 0; tv = 0; tr = 0; ca = 0; nh = 0;
      this.hostErrors = 'network connectivity unavailable: ' + String(error);
    }
    try {
      let state = sim.getSimStateSync(0);
      let mapped: number[] = this.mapSimState(state);
      ss = mapped[0];
      ssv = mapped[1];
      telv = 1;
    } catch (error) {
      this.hostErrors = 'SIM state unavailable: ' + String(error);
    }
    try {
      let tech = radio.getRadioTechSync(0);
      telv = 1;
      let mapped: number = this.mapRadioTech(tech.psRadioTech);
      if (mapped >= 0) { nt = mapped; ntv = 1; }
    } catch (error) {
      this.hostErrors = 'radio technology unavailable: ' + String(error);
    }
    try {
      let name: string = radio.getOperatorNameSync(0);
      telv = 1;
      if (name.length > 0) { operatorName = name; onav = 1; }
    } catch (error) {
      this.hostErrors = 'operator name unavailable: ' + String(error);
    }
    if (this.telephonyPrefetchOk) { telv = 1; }
    if (this.operatorNumeric.length > 0) { operatorNumeric = this.operatorNumeric; onv = 1; }
    if (this.operatorNameCache.length > 0) { operatorName = this.operatorNameCache; onav = 1; }
    let payload: string = 'cv=' + cv + '&hd=' + hd + '&co=' + co + '&av=' + av + '&me=' + me +
      '&ty=' + ty + '&tv=' + tv + '&tr=' + tr + '&ca=' + ca + '&nh=' + nh +
      '&telv=' + telv + '&nt=' + nt + '&ntv=' + ntv + '&ss=' + ss + '&ssv=' + ssv +
      '&pt=' + pt + '&ptv=' + ptv + '&onv=' + onv + '&onav=' + onav +
      '&on=' + encodeURIComponent(operatorNumeric) + '&ona=' + encodeURIComponent(operatorName);
    probe.hostReplyNetwork(id, 0, payload);
  }
  private mapOrientation(orientation: number): number {
    // Android Configuration.ORIENTATION_PORTRAIT=1 / LANDSCAPE=2 / UNDEFINED=0.
    if (orientation === display.Orientation.PORTRAIT || orientation === display.Orientation.PORTRAIT_INVERTED) { return 1; }
    if (orientation === display.Orientation.LANDSCAPE || orientation === display.Orientation.LANDSCAPE_INVERTED) { return 2; }
    return 0;
  }
  private async refreshTelephony(): Promise<void> {
    try {
      let state = await radio.getNetworkState(0);
      this.operatorNumeric = state.plmnNumeric ? state.plmnNumeric : '';
      this.telephonyPrefetchOk = true;
      if (state.longOperatorName) { this.operatorNameCache = state.longOperatorName; }
      else if (state.shortOperatorName) { this.operatorNameCache = state.shortOperatorName; }
      else { this.operatorNameCache = ''; }
    } catch (error) {
      this.operatorNumeric = '';
      this.operatorNameCache = '';
      this.telephonyPrefetchOk = false;
      this.hostErrors = 'telephony prefetch unavailable: ' + String(error);
    }
    try {
      let name: string = radio.getOperatorNameSync(0);
      if (name.length > 0) { this.operatorNameCache = name; }
    } catch (error) {
      this.hostErrors = 'operator name prefetch unavailable: ' + String(error);
    }
  }
  private async refreshDisplaySnapshot(): Promise<void> {
    try {
      let metrics = display.getDefaultDisplaySync();
      let density: number = metrics.densityPixels;
      if (!(density > 0)) { throw new Error('display densityPixels is not positive'); }
      let widthPixels: number = metrics.width;
      let heightPixels: number = metrics.height;
      if (!(widthPixels > 0) || !(heightPixels > 0)) { throw new Error('display width/height is not positive'); }
      if (!(metrics.densityDPI > 0)) { throw new Error('display densityDPI is not positive'); }
      let visibleWidth: number = widthPixels;
      let visibleHeight: number = heightPixels;
      let windowSource: string = 'display-only';
      try {
        let mainWindow = await window.getLastWindow(this.context());
        let properties = mainWindow.getWindowProperties();
        if (properties.windowRect.width > 0 && properties.windowRect.height > 0) {
          visibleWidth = properties.windowRect.width;
          visibleHeight = properties.windowRect.height;
          windowSource = 'windowRect';
        }
      } catch (error) {
        windowSource = 'display-only: ' + String(error);
      }
      let widthDp: number = Math.round(visibleWidth / density);
      let heightDp: number = Math.round(visibleHeight / density);
      let smallestDp: number = Math.min(widthDp, heightDp);
      let orientation: number = this.mapOrientation(metrics.orientation);
      let source: string = '@ohos.display.getDefaultDisplaySync + @ohos.window.getLastWindow/getWindowProperties(' + windowSource + ')';
      probe.setDisplaySnapshot(widthPixels, heightPixels, metrics.densityDPI, orientation, widthDp, heightDp, smallestDp,
        density, metrics.scaledDensity, metrics.xDPI, metrics.yDPI, source);
      this.hostInfo = 'Display snapshot cached: ' + widthPixels + 'x' + heightPixels + ' px, ' +
        widthDp + 'x' + heightDp + ' dp, density ' + density + ', source ' + windowSource;
    } catch (error) {
      probe.clearDisplaySnapshot();
      this.hostInfo = 'Display snapshot failed; runtime fallback retained: ' + String(error);
    }
  }
  private async runHostProbe(): Promise<void> {
    if (this.sdkBusy) { return; }
    this.sdkBusy = true;
    this.result = 'Probing host bridge...';
    try {
      await this.refreshTelephony();
      await this.refreshDisplaySnapshot();
      let report: string = await probe.hostProbe('all');
      // The C probe body still carries a legacy annotation.  Correct it at the
      // display layer (probe.cpp outside HostStatus is not part of this change).
      report = report.replace('(public @ohos.settings exposes no USER_SECURE domain constant; host reports absent)',
        '(public @ohos.settings exposes no USER_SECURE domain constant; unsupported domain -> -1 backend failure, fail-closed)');
      this.result = SETTINGS_CAPABILITY_SUMMARY + '\n' + report;
      if (this.hostErrors.length > 0) { this.result += '\nlast host warning: ' + this.hostErrors; }
    } catch (error) {
      this.result = 'Host probe failed: ' + String(error);
    } finally {
      this.sdkBusy = false;
    }
  }
  private async runUnityBatch(original: boolean = false, audio: boolean = false): Promise<void> {
    if (this.sdkBusy) { return; }
    this.sdkBusy = true;
    this.result = 'Checking original game dependency symbols...';
    try {
      let context = getContext(this) as common.UIAbilityContext;
      let report: string = audio ? await probe.audioBridge() : (original ? await probe.originalMain() : await probe.unityBatch());
      let name: string = audio ? '/audio-bridge.txt' : (original ? '/original-main.txt' : '/unity-batch.txt');
      this.writeDiagnosticReport(context.filesDir + name, report);
      this.result = report;
    } catch (error) { this.result = 'Unity batch failed: ' + String(error); }
    finally { this.sdkBusy = false; }
  }
  private async runSdkTrace(jni: boolean = false, login: boolean = false): Promise<void> {
    if (this.sdkBusy) { return; }
    this.sdkBusy = true;
    this.result = 'Preparing original SDK DEX files...';
    try {
      let context = getContext(this) as common.UIAbilityContext;
      let directory: string = context.filesDir + '/sdkdex';
      if (!fs.accessSync(directory)) { fs.mkdirSync(directory, true); }
      this.result = 'Reading app VM diagnostics...';
      let report: string = login ? await probe.loginTrace(directory) : (jni ? await probe.jniBatch(directory) : await probe.sdkTrace(directory));
      let name: string = login ? '/login-trace.txt' : (jni ? '/jni-batch.txt' : '/sdk-trace.txt');
      this.writeDiagnosticReport(context.filesDir + name, report);
      this.result = report + '\nFull report: ' + name;
    } catch (error) {
      this.result = 'SDK trace failed: ' + String(error);
    } finally { this.sdkBusy = false; }
  }
  private runNativeProbe(): void {
    if (this.sdkBusy) { return; }
    this.sdkBusy = true;
    this.result = 'Probing original native libraries (dlopen/dlsym only)...';
    try {
      let context = getContext(this) as common.UIAbilityContext;
      let report: string = probe.nativeProbeRun();
      this.writeDiagnosticReport(context.filesDir + '/native-probe.txt', report);
      this.result = report + '\nFull report: /native-probe.txt';
    } catch (error) {
      this.result = 'Native probe failed: ' + String(error);
    } finally { this.sdkBusy = false; }
  }
  private runElfLoaderProbe(): void {
    if (this.sdkBusy) { return; }
    this.sdkBusy = true;
    this.result = 'Loading original libraries with the custom ELF64 loader...';
    try {
      let context = getContext(this) as common.UIAbilityContext;
      let report: string = probe.elfLoaderProbe();
      this.writeDiagnosticReport(context.filesDir + '/elf-loader-probe.json', report);
      this.result = report + '\nFull report: /elf-loader-probe.json';
    } catch (error) {
      this.result = 'ELF loader probe failed: ' + String(error);
    } finally { this.sdkBusy = false; }
  }
  aboutToAppear(): void {
    this.loadLoginHistory();
    try {
      probe.setDeviceSnapshot(deviceInfo.brand, deviceInfo.manufacture,
        deviceInfo.productModel, deviceInfo.deviceType, deviceInfo.productModel,
        deviceInfo.osFullName, deviceInfo.abiList, deviceInfo.hardwareModel);
    } catch (ignored) { }
    this.status = probe.status();
    probe.subscribe((status: string) => { this.receiveNativeEvent(status); });
    this.genericWebSyncNeeded = true;
    this.retryGenericWebCommands();
    if (this.genericWebRetryTimer !== -1) { clearInterval(this.genericWebRetryTimer); }
    this.genericWebRetryTimer = setInterval(() => { this.retryGenericWebCommands(); }, 500);
    this.registerHostBridge();
    try {
      let loginState: LoginPanelMessage = JSON.parse(probe.loginGetState()) as LoginPanelMessage;
      this.loginVisible = loginState.required === true;
      this.loginRequestId = loginState.activeRequestId ?? '';
      this.applyLoginState(loginState);
    } catch (ignored) { }
    this.initializeHostSnapshots();
    this.loginRefreshTimer = setInterval(() => { this.refreshLoginState(); }, 500);
    if (ENABLE_ORIENTATION_SELF_TEST) {
      this.probeOrientationSelfTest().then(() => {}).catch((e: BusinessError) => {
        this.probeLog('HOST_ORIENTATION_SELFTEST dispatch_failed code=' + String(e.code));
      });
    }
  }
  aboutToDisappear(): void {
    if (this.splashFadeTimer !== -1) { clearTimeout(this.splashFadeTimer); this.splashFadeTimer = -1; }
    if (this.genericWebRetryTimer !== -1) { clearInterval(this.genericWebRetryTimer); this.genericWebRetryTimer = -1; }
    this.genericWebSyncNeeded = true;
    this.genericWebPendingEvaluations = [];
    for (let page of this.genericWebPages.slice()) { this.removeGenericWeb(page.id); }
    this.loginPassword = '';
    this.clearLoginTimer();
    if (this.loginRefreshTimer !== -1) { clearInterval(this.loginRefreshTimer); this.loginRefreshTimer = -1; }
    if (this.batterySequence !== 0) { probe.batteryUpdate(this.batterySequence, false, true, 0, 1, 1, 0, 0, 0, 0, ''); }
    this.batteryStop(this.batterySequence);
    try { probe.unregisterHostHandler(); } catch (e) { }
    try { probe.clearDisplaySnapshot(); } catch (e) { }
    probe.unsubscribe();
    probe.stop();
  }
  build() { Stack({ alignContent: Alignment.Center }) {
    Column({ space: this.unityPresentation ? 0 : 8 }) {
    Text('Native Probe').visibility(this.unityPresentation ? Visibility.None : Visibility.Visible)
    Text('Diagnostic mode: every touch is logged. UI rendering may coalesce updates.').fontSize(12).visibility(this.unityPresentation ? Visibility.None : Visibility.Visible)
    if (this.guestMounted) {
      if (this.guestVertical) { Column() {}.width('100%').height('45%').backgroundColor(Color.White) }
      else { Row() {}.width('100%').height('45%').backgroundColor(Color.White) }
    } else {
      XComponent({ id: 'surface', type: XComponentType.SURFACE, libraryname: 'probe' }).id('unity-host-surface').focusable(true).onFocus(() => { this.unitySurfaceFocused = true; }).onBlur(() => { this.unitySurfaceFocused = false; }).width('100%').height(this.unityPresentation ? '100%' : '45%').onLoad(() => { this.probeLog('XCOMPONENT_MOUNT id=unity-host-surface'); this.startGameOnce(); }).onDestroy(() => { this.probeLog('XCOMPONENT_UNMOUNT id=unity-host-surface'); })
    }
    }.width('100%').height('100%')
    if (this.vmBootBlocking && this.loadingSplashDisabled) {
      Column({ space: 12 }) {
        Text(this.pageGeometryStatus).fontSize(14).fontColor(Color.White).maxLines(3)
        Text(this.vmBootStatus).fontSize(22).fontColor(Color.White).maxLines(8)
      }.width('88%').padding(24).backgroundColor('#DD202030').borderRadius(12)
    }
    // Diagnostics hit targets must never cover the game's Back button.
    if (!this.unityPresentation) {
    Row() {}.id('diagnostics-gesture').width(120).height(120)
      .position({ x: 0, y: 0 })
      .gesture(LongPressGesture({ repeat: false, duration: 1000 }).onAction(() => {
        this.diagnosticsVisible = !this.diagnosticsVisible;
      }))
    } else {
      Row() {}.id('diagnostics-gesture-game').width(50).height(50)
        .position({ x: '47%', y: 8 })
        .gesture(LongPressGesture({ repeat: false, duration: 1000 }).onAction(() => {
          this.diagnosticsVisible = !this.diagnosticsVisible;
        }))
    }
    if (SHOW_LOGIN_TEST_ENTRY) {
      Button('登录').id('login-open-test').width(88).height(36).fontSize(14)
        .position({ x: '88%', y: 8 }).backgroundColor('#88555555')
        .onClick(() => { this.openLoginPanel(); })
    }
    if (this.diagnosticsVisible) {
    Column({ space: 8 }) {
    Text('关于 / About\n' + probe.apkVersionInfo()).fontSize(14)
    Text(this.pageGeometryStatus).fontSize(14).maxLines(3)
    Text(this.vmBootStatus).fontSize(14).maxLines(5)
    Text(this.status).fontSize(12)
    Text(this.hostInfo).fontSize(12).maxLines(3).textOverflow({ overflow: TextOverflow.Ellipsis })
    Text(this.result).fontSize(12).maxLines(8).textOverflow({ overflow: TextOverflow.Ellipsis })
    Row({ space: 8 }) {
      Button('Check audio bridge').enabled(!this.sdkBusy).onClick(() => { this.runUnityBatch(false,true); })
    }
    Row({ space: 8 }) {
      Button(this.sdkBusy ? 'SDK trace running...' : 'Trace original SDK').enabled(!this.sdkBusy).onClick(() => { this.runSdkTrace(); })
      Button('Trace JNI bridge').enabled(!this.sdkBusy).onClick(() => { this.runSdkTrace(true); })
      Button('Trace login entry').enabled(!this.sdkBusy).onClick(() => { this.runSdkTrace(false,true); })
    }
    Row({ space: 8 }) {
      Button('Play pulse').onClick(() => { this.result = probe.play(); })
      Button('Stop').onClick(() => { this.result = probe.stop(); })
      Button('Host status').onClick(() => {
        let report: string = SETTINGS_CAPABILITY_SUMMARY + '\n' + probe.hostStatus();
        if (this.hostErrors.length > 0) { report += '\nlast host warning: ' + this.hostErrors; }
        this.result = report;
      })
    }
    if (this.diagnosticsVisible) {
      Button('登录面板（排障）').id('login-panel-debug-open').onClick(() => { this.openLoginPanel(); })
      Button('收起排障').onClick(() => { this.diagnosticsVisible = false; })
    }
    }.width('95%').padding(12).backgroundColor('#F5F5F5').borderRadius(12)
    }
    if (this.loadingSplashDisabled && this.unityPresentation && !this.loginVisible && this.genericWebPages.length === 0 &&
      !this.titleInitRequired && this.titleProcess >= 2 && this.titleProcess < 15) {
      Text('NOW LOADING…').id('title-progress-feedback').fontSize(24).fontColor(Color.White)
        .padding(16).backgroundColor('#BB202020').borderRadius(12)
        .position({ x: '40%', y: '86%' }).hitTestBehavior(HitTestMode.None)
        .onAppear(() => { this.probeLog('TAP_PROGRESS_VISIBLE process=' + String(this.titleProcess) +
          ' host_epoch_ms=' + String(Date.now()) + ' source=original_title_state'); })
    }
    if (this.unityPresentation && this.titleInitRequired && this.genericWebPages.length === 0) {
      Text('SDK 尚未初始化（3004）。请重新打开应用，在协议网页内点击「同意」后再登录。')
        .id('title-init-required').fontSize(18).fontColor(Color.White).padding(16)
        .backgroundColor('#DD402020').borderRadius(12).width('80%')
        .position({ x: '10%', y: '80%' }).hitTestBehavior(HitTestMode.None)
    }
    if (this.loginVisible) {
      Row() {}.id('login-dialog-mask').width('100%').height('100%')
        .backgroundColor('#88000000').hitTestBehavior(HitTestMode.Block).onClick(() => {})
      if (!this.loginInputRequested && !this.sdkSavedKnown) {
        Column({ space: 12 }) {
          Text('正在读取 SDK 保存的账号').fontColor(Color.Black)
          Button('更换账号').onClick(() => { this.switchLoginAccount(); })
        }.width(400).constraintSize({ maxWidth: '90%' }).padding(16).backgroundColor('#F5F5F5').borderRadius(12)
      } else if (!this.loginInputRequested && this.sdkSavedPresent) {
        Column({ space: 12 }) {
          Button('登录已有账号（' + this.sdkSavedName + '）').id('login-saved-account')
            .enabled(this.loginReady && !this.loginWorkerBusy).onClick(() => { this.submitSavedLogin(); })
          Button('更换账号').id('login-change-account')
            .enabled(!this.loginWorkerBusy).onClick(() => { this.switchLoginAccount(); })
          if (this.loginResult.length > 0) { Text(this.loginResult).fontColor(Color.Black) }
        }.id('login-saved-dialog').width(400).constraintSize({ maxWidth: '90%' }).padding(16)
          .backgroundColor('#F5F5F5').borderRadius(12)
      } else {
      Column({ space: 8 }) {
        Row() {
          Text('B站账号登录').fontSize(20).fontColor(Color.Black)
          Blank()
          Button('关闭').id('login-panel-close').fontSize(12).height(30).onClick(() => {
            this.loginVisible = false; this.loginPassword = '';
          })
        }.width('100%')
        Text(this.loginHistoryStatus).fontSize(12).fontColor(Color.Black).width('100%')
        Button('更换账号').id('login-switch-account').height(30).fontSize(12)
          .enabled(!this.loginBusy && !this.loginWorkerBusy).onClick(() => { this.switchLoginAccount(); })
        if (this.accountListVisible) {
          ForEach(this.savedLoginAccounts, (name: string) => {
            Button(name).height(30).fontSize(12).onClick(() => {
              this.loginAccount = name; this.loginPassword = ''; this.accountListVisible = false;
            })
          }, (name: string): string => name)
        }
        TextInput({ text: this.loginAccount, placeholder: '账号' }).id('login-account')
          .contentType(ContentType.USER_NAME).enableAutoFill(true)
          .onChange((value: string) => { this.loginAccount = value; this.passwordVaultStatus = ''; })
        TextInput({ text: this.loginPassword, placeholder: '密码' }).id('login-password').type(InputType.Password)
          .showPasswordIcon(true).contentType(ContentType.PASSWORD).enableAutoFill(true)
          .onChange((value: string) => { this.loginPassword = value; this.passwordVaultStatus = ''; })
        Text(this.loginReady ? this.loginPhase : '运行环境未就绪').id('login-phase')
          .fontSize(13).fontColor(Color.Black).width('100%')
        Button(this.loginBusy ? '提交中' : '登录').id('login-submit')
          .enabled(this.loginReady && !this.loginBusy && this.loginAccount.length > 0 && this.loginPassword.length > 0)
          .onClick(() => { this.submitLogin(); })
        Text(this.loginResult).id('login-response').fontSize(13).fontColor(Color.Black).width('100%')
        Row({ space: 8 }) {
          Button(this.passwordVaultBusy ? '等待系统处理' : '保存到密码保险箱').id('password-vault-save')
            .height(32).fontSize(12).layoutWeight(1)
            .enabled(!this.passwordVaultBusy && !this.passwordVaultFillBusy && this.loginAccount.length > 0 && this.loginPassword.length > 0)
            .onClick(() => { this.requestPasswordVaultSave(); })
          Button(this.passwordVaultFillBusy ? '等待系统填充' : '从密码保险箱填充').id('password-vault-fill')
            .height(32).fontSize(12).layoutWeight(1)
            .enabled(!this.passwordVaultBusy && !this.passwordVaultFillBusy)
            .onClick(() => { this.requestPasswordVaultFill(); })
        }.width('100%')
        if (this.passwordVaultStatus.length > 0) {
          Text(this.passwordVaultStatus).id('password-vault-status').fontSize(12).fontColor(Color.Black).width('100%')
        }
        if (this.captchaNeeded) {
          Button('去验证').id('captcha-open').enabled(this.captchaUrl.length > 0)
            .onClick(() => { this.captchaVisible = true; })
        }
      }.id('login-dialog-card').width(400).constraintSize({ maxWidth: '90%' }).padding(16)
        .backgroundColor('#F5F5F5').borderRadius(12).hitTestBehavior(HitTestMode.Default).onClick(() => {})
      }
    }
    if (this.captchaVisible && this.captchaUrl.length > 0) {
      Row() {}.id('captcha-dialog-mask').width('100%').height('100%')
        .backgroundColor('#88000000').hitTestBehavior(HitTestMode.Block).onClick(() => {})
      Column() {
        Row() {
          Text('验证码验证').fontSize(18).fontColor(Color.Black)
          Blank()
          Button('关闭').id('captcha-close').height(30).fontSize(12).onClick(() => {
            if (this.captchaActive) { this.deliverCaptchaResult('', 'goBackWithResult'); }
            else { this.webPlatform.detach(this.captchaWebRef, this.captchaController); this.captchaVisible = false; }
          })
        }.width('100%').padding(8)
        Web({ src: 'about:blank', controller: this.captchaController }).id('captcha-web')
          .javaScriptAccess(true).domStorageAccess(true)
          .onControllerAttached(() => {
            try { this.webPlatform.attach(this.captchaWebRef, this.captchaController); }
            catch (error) { this.probeLog('WEB_COOKIE captcha_attach status=-1'); }
          })
          .javaScriptProxy({ object: this.captchaProxy, name: this.captchaInterface,
            methodList: ['finishWithResult', 'goBackWithResult'], controller: this.captchaController })
          .onPageEnd((event) => {
            if (!this.captchaNavigationStarted && event.url === 'about:blank') {
              this.captchaNavigationStarted = true;
              try { this.captchaController.loadUrl(this.captchaUrl); }
              catch (error) { this.probeLog('CAPTCHA_WEB_LOAD_ERROR'); }
              return;
            }
            this.webPlatform.observePage(this.captchaWebRef, this.captchaController, event.url);
            this.probeLog('CAPTCHA_WEB_PAGE_END');
          })
          .onErrorReceive(() => { this.probeLog('CAPTCHA_WEB_LOAD_ERROR'); })
          .width('100%').layoutWeight(1)
      }.id('captcha-dialog-card').width(400).height('88%')
        .constraintSize({ maxWidth: '90%', maxHeight: 600 }).borderRadius(12).clip(true)
        .backgroundColor(Color.White).hitTestBehavior(HitTestMode.Default).onClick(() => {})
    }
    ForEach(this.genericWebPages, (page: GenericWebPage) => {
      GenericWebOverlay({ page: page, isTop: page.serial === this.genericWebPages[this.genericWebPages.length - 1].serial,
        onClose: (id: string, force: boolean): void => { this.closeGenericWeb(id, force); },
        onNative: (id: string, action: string, payload: string): number => {
          return this.genericWebCommand(id, action, payload);
        } })
    }, (page: GenericWebPage): string => { return page.id + ':' + String(page.serial); })
    if (!this.loadingSplashDisabled && this.splashVisible && this.loadingError.length === 0) {
      Column({ space: 24 }) {
        Row({ space: 9 }) {
          Row() {}.width(9).height(28).borderRadius(5).backgroundColor('#515968')
          Row() {}.width(9).height(48).borderRadius(5).backgroundColor('#515968')
          Row() {}.width(9).height(36).borderRadius(5).backgroundColor('#515968')
        }.height(56).alignItems(VerticalAlign.Center)
        Stack({ alignContent: Alignment.Start }) {
          Row() {}.width('100%').height('2px').backgroundColor('#ECEEF1')
          Row() {}.width(this.splashProgress + '%').height('2px').backgroundColor('#697585')
        }.width(220).height('2px')
        Text(this.splashLabel).fontSize('14fp').fontColor('#777D86').maxLines(1)
      }.id('loading-splash').width('100%').height('100%').justifyContent(FlexAlign.Center)
        .backgroundColor('#FFFFFF').opacity(this.splashOpacity).hitTestBehavior(HitTestMode.None)
        .onAppear(() => { this.probeLog('SPLASH_VISIBLE wall=' + Date.now()); this.updateSplash(); })
    }
    if (!this.loadingSplashDisabled && this.loadingError.length > 0) {
      Column({ space: 16 }) {
        Text('暂时无法启动').fontSize(20).fontColor('#333B48')
        Text(this.loadingError).fontSize('14fp').fontColor('#777D86')
        Row({ space: 16 }) {
          Button('重试').id('loading-retry').onClick(() => { this.retrySplash(); })
          Button('查看日志').id('loading-view-logs').onClick(() => { this.splashLogs = !this.splashLogs; })
        }
        if (this.splashLogs) {
          Text(this.vmBootStatus.slice(0, 1200)).id('loading-error-log').fontSize(12).fontColor('#555D68')
            .maxLines(12).width('80%')
        }
      }.id('loading-error-card').width('100%').height('100%').justifyContent(FlexAlign.Center)
        .backgroundColor('#FFFFFF').hitTestBehavior(HitTestMode.Transparent)
    }
  }.id('login-host-page').width('100%').height('100%') }
}
