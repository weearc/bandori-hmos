import { UIAbility, Want } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import probe from 'libprobe.so';
import hilog from '@ohos.hilog';
import { fileIo as fs } from '@kit.CoreFileKit';
import i18n from '@ohos.i18n';
export default class EntryAbility extends UIAbility {
  private importSelftestReset: boolean = false;
  private pageSizeOverride: string = '';
  private pageMapProbeOnly: boolean = false;
  private destroyed: boolean = false;
  private heartbeatTimer: number = -1;
  private assetProgressTimer: number = -1;
  private refreshTimeZoneSnapshot(): void {
    // Official i18n API returns the device's configured zone when ID is omitted.
    try {
      let zone: i18n.TimeZone = i18n.getTimeZone();
      let id: string = zone.getID();
      let offset: number = zone.getRawOffset();
      probe.setTimeZoneSnapshot(id, offset);
      hilog.info(0xFF00, 'NativeProbeLife', 'HOST_TIMEZONE snapshot=true id=%{public}s rawOffsetMs=%{public}d', id, offset);
    } catch (error) {
      hilog.error(0xFF00, 'NativeProbeLife', 'HOST_TIMEZONE snapshot=false');
    }
  }
  private async createAppVm(): Promise<void> {
    let directory: string = this.context.filesDir + '/sdkdex';
    if (!fs.accessSync(directory)) { fs.mkdirSync(directory, true); }
    // Bounded diagnostic Want: app writes its own sandbox; no shell permission bypass.
    if (this.pageSizeOverride !== '') {
      let pageFile = fs.openSync(directory + '/guest-page-size', fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      try { fs.writeSync(pageFile.fd, this.pageSizeOverride + '\n'); } finally { fs.closeSync(pageFile); }
    }
    AppStorage.setOrCreate('pageGeometryStatus', probe.pageGeometry(directory));
    AppStorage.setOrCreate('vmBootBlocking', true);
    AppStorage.setOrCreate('vmBootStatus', 'vm_last_step=copy_base_apk\n正在解压游戏资源 0%（准备 APK）');
    for (let name of ['guest-profile.cfg']) {
      let bytes = await this.context.resourceManager.getRawFileContent('sdkdex/' + name);
      if (this.destroyed) { return; }
      let file = fs.openSync(directory + '/' + name, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      try {
        let buffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
        if (fs.writeSync(file.fd, buffer) !== bytes.byteLength) { throw new Error('E-EXTRACT-WRITE step=copy_' + name); }
      } finally { fs.closeSync(file); }
    }
    if (this.destroyed) { return; }
    this.assetProgressTimer = setInterval(() => { AppStorage.setOrCreate('vmBootStatus', probe.nativeAssetsStatus()); }, 250);
    let opened: boolean = false;
    try {
      let raw = await this.context.resourceManager.getRawFd('sdkdex/base.apk');
      opened = true;
      await probe.prepareNativeAssets(directory, raw.fd, raw.offset, raw.length, this.importSelftestReset);
      AppStorage.setOrCreate('vmBootStatus', probe.nativeAssetsStatus());
    } finally {
      clearInterval(this.assetProgressTimer); this.assetProgressTimer = -1;
      if (opened) { await this.context.resourceManager.closeRawFd('sdkdex/base.apk'); }
    }
    AppStorage.setOrCreate('vmBootBlocking', false);
    if (this.destroyed) { return; }
    if (this.pageMapProbeOnly) {
      let report: string = probe.pageMapProbe(directory);
      AppStorage.setOrCreate('vmBootBlocking', true);
      AppStorage.setOrCreate('vmBootStatus', 'vm_last_step=page_map_probe_complete\nMapping only; constructors/JNI/gameplay not run.\n' + report.slice(-160));
      return;
    }
    hilog.info(0xFF00, 'NativeProbeLife', 'APP_VM create requested source=EntryAbility.onCreate wall=%{public}s', String(Date.now()));
    // Completion belongs to onDestroy; diagnostics never await/own this work.
    probe.appVmCreate(directory).then(() => {
      hilog.info(0xFF00, 'NativeProbeLife', 'APP_VM owner released');
    }).catch((error: Error) => {
      AppStorage.setOrCreate('vmBootBlocking', true);
      AppStorage.setOrCreate('vmBootStatus', probe.nativeAssetsStatus() + '\n' + String(error));
      hilog.error(0xFF00, 'NativeProbeLife', 'APP_VM start failed %{public}s', String(error));
    });
  }
  onCreate(want: Want): void {
    this.importSelftestReset = want.parameters?.['apkImportSelftestReset'] === true;
    let requestedPage = want.parameters?.['guestPageSize'];
    if (typeof requestedPage === 'string' && ['auto', '4096', '16384', '65536'].includes(requestedPage)) { this.pageSizeOverride = requestedPage; }
    this.pageMapProbeOnly = want.parameters?.['pageMapProbeOnly'] === true;
    this.heartbeatTimer = setInterval(() => {
      let bootFailure: string = probe.appVmHeartbeat();
      if (bootFailure.startsWith('E-GC1-SELFTEST-FAIL')) {
        AppStorage.setOrCreate('vmBootBlocking', true);
        AppStorage.setOrCreate('vmBootStatus', bootFailure);
      }
    }, 500);
    hilog.info(0xFF00, 'NativeProbeLife', 'HOST_LIFECYCLE onCreate wall=%{public}s', String(Date.now()));
    // Populate before any VM worker can execute Java; never guess on failure.
    this.refreshTimeZoneSnapshot();
    this.createAppVm().catch((error: Error) => {
      AppStorage.setOrCreate('vmBootBlocking', true);
      let detail: string = String(error);
      if (detail.indexOf('E-EXTRACT-') < 0) { detail = 'E-EXTRACT-OPEN step=prepare_resource ' + detail; }
      AppStorage.setOrCreate('vmBootStatus', probe.nativeAssetsStatus() + '\n' + detail);
      hilog.error(0xFF00, 'NativeProbeLife', 'APP_VM resource preparation failed %{public}s', String(error));
    });
  }
  onForeground(): void {
    this.refreshTimeZoneSnapshot();
    hilog.info(0xFF00, 'NativeProbeLife', 'HOST_LIFECYCLE onForeground wall=%{public}s', String(Date.now()));
    probe.appVmForeground(true);
  }
  onWindowStageCreate(stage: window.WindowStage): void {
    hilog.info(0xFF00, 'NativeProbeLife', 'HOST_LIFECYCLE onWindowStageCreate wall=%{public}s', String(Date.now()));
    stage.getMainWindow().then(async (main: window.Window) => {
      await main.setPreferredOrientation(window.Orientation.LANDSCAPE);
      hilog.info(0xFF00, 'NativeProbeLife', 'HOST_CONTENT_LOAD source=window_stage wall=%{public}s', String(Date.now()));
      await stage.loadContent('pages/Index');
    }).catch((error: Error) => {
      hilog.error(0xFF00, 'NativeProbeLife', 'HOST_CONTENT_LOAD source=window_stage_fallback wall=%{public}s error=%{public}s', String(Date.now()), String(error));
      stage.loadContent('pages/Index');
    });
  }
  onBackground(): void {
    hilog.info(0xFF00, 'NativeProbeLife', 'HOST_LIFECYCLE onBackground wall=%{public}s', String(Date.now()));
    probe.appVmForeground(false);
  }
  onDestroy(): void {
    this.destroyed = true;
    if (this.assetProgressTimer !== -1) { clearInterval(this.assetProgressTimer); this.assetProgressTimer = -1; }
    if (this.heartbeatTimer !== -1) { clearInterval(this.heartbeatTimer); this.heartbeatTimer = -1; }
    probe.appVmDestroy();
    hilog.info(0xFF00, 'NativeProbeLife', 'HOST_LIFECYCLE onDestroy wall=%{public}s', String(Date.now()));
    probe.stop();
  }
  onWindowStageDestroy(): void { hilog.info(0xFF00, 'NativeProbeLife', 'HOST_LIFECYCLE onWindowStageDestroy wall=%{public}s', String(Date.now())); }
}
