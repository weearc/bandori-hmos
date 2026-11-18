// Milestones only. No timer-derived progress and no extrapolation.
export class LoadingProgress {
  value: number = 0;
  label: string = '检查安装包';
  presented: boolean = false;

  update(assets: string, prepared: boolean, snapshot: string): void {
    let candidate: number = this.value;
    let entries: RegExpMatchArray | null = assets.match(/processed=(\d+) total=(\d+)/);
    if (entries !== null && Number(entries[2]) > 0 && !prepared) {
      let done: number = Math.min(Number(entries[1]), Number(entries[2]));
      candidate = 70 * done / Number(entries[2]);
      if (candidate >= this.value) { this.label = '解出资源 ' + done + '/' + entries[2]; }
    }
    if (prepared) { candidate = Math.max(candidate, 70); if (this.value <= 70) { this.label = '加载原生库'; } }
    if (snapshot.indexOf('stage=2 ') === 0) { candidate = Math.max(candidate, 90); if (this.value <= 90) { this.label = '启动引擎'; } }
    let count: RegExpMatchArray | null = snapshot.match(/submissions=(\d+)/);
    if (count !== null && Number(count[1]) > 0) {
      candidate = 100; this.label = '首帧'; this.presented = true;
    }
    this.value = Math.max(this.value, candidate);
  }
}
