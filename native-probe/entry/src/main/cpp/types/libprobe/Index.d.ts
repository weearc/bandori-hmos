export const test: () => string;
export const loginSubmit: (requestId: string, account: string, password: string) => number;
export const loginSavedLogin: (requestId: string) => number;
export const loginGetState: () => string;
export const status: () => string;
export const play: () => string;
export const stop: () => string;
export const subscribe: (callback: (status: string) => void) => void;
export const unsubscribe: () => void;
export const androidElf: () => string;
export const androidLibc: () => string;
export const sdkTrace: (directory: string) => Promise<string>;
export const nativeDependencies: () => string;
export const unityBatch: () => Promise<string>;
export const originalMain: () => Promise<string>;
export const audioBridge: () => Promise<string>;
export const jniBatch: (directory: string) => Promise<string>;
export const loginTrace: (directory: string) => Promise<string>;
export const appVmCreate: (directory: string) => Promise<string>;
export const prepareNativeAssets: (directory: string, apkFd: number, offset: number, length: number, selftestReset?: boolean) => Promise<string>;
export const nativeAssetsStatus: () => string;
export const apkVersionInfo: () => string;
export const appVmWindowReady: () => string;
export const appVmHeartbeat: () => string;
export const loadingStatus: () => string;
export const audioLatencyStatus: () => string;
export const appVmForeground: (foreground: boolean) => string;
export const appVmDestroy: () => string;
export const nativeProbeRun: (directory?: string) => string;
export const elfLoaderProbe: () => string;
export const injectTap: (x?: number, y?: number) => string;
export const setDeviceSnapshot: (brand: string, manufacturer: string, model: string, device: string, product: string, osFullName: string, abis?: string, hardware?: string) => void;
export const setTimeZoneSnapshot: (id: string, rawOffsetMs: number) => void;
export const ackGuestView: (sequence: number, accepted: boolean) => void;

export const batteryUpdate: (sequence: number, accepted: boolean, delivered: boolean, level: number, status: number, health: number, plugged: number, temperature: number, voltage: number, present: number, technology: string) => void;

export const registerHostHandler: (callback: (message: string) => void) => void;
export const unregisterHostHandler: () => void;
export const hostReply: (id: number, status: number, value: string) => void;
export const hostReplyNetwork: (id: number, status: number, payload: string) => void;

export const setDisplaySnapshot: (
  widthPixels: number,
  heightPixels: number,
  densityDpi: number,
  orientation: number,
  screenWidthDp: number,
  screenHeightDp: number,
  smallestScreenWidthDp: number,
  density: number,
  scaledDensity: number,
  xdpi: number,
  ydpi: number,
  source: string
) => void;
export const clearDisplaySnapshot: () => void;

export const hostProbe: (mode: string) => Promise<string>;
export const hostStatus: () => string;
export const loginCaptchaResult: (requestId: string, payload: string, method: string) => number;

export const webviewCommand: (id: string, action: string, payload: string) => number;

export const pageGeometry: (directory: string) => string;
export const pageMapProbe: (directory: string) => string;
