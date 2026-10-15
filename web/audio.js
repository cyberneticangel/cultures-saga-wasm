// AudioWorklet: plays the stereo s16 ring buffer the game's mixer fills (see worker.js).
class RingPlayer extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ctl = null;
    this.gain = 1;
    this.port.onmessage = e => {
      if ('gain' in e.data) { this.gain = e.data.gain; return; }
      const { shared, audioOff, frames } = e.data;
      this.ctl = new Int32Array(shared, 0, 2048);
      this.ring = new Int16Array(shared, audioOff, frames * 2);
      this.frames = frames;
      Atomics.store(this.ctl, 12, 1);
    };
  }
  process(_inputs, outputs) {
    const out = outputs[0];
    const l = out[0], r = out[1] || out[0];
    if (!this.ctl) return true;
    const g = this.gain / 32768;
    let rd = Atomics.load(this.ctl, 6);
    const wr = Atomics.load(this.ctl, 5);
    for (let i = 0; i < l.length; i++) {
      if (rd < wr) {
        const o = (rd % this.frames) * 2;
        l[i] = this.ring[o] * g;
        r[i] = this.ring[o + 1] * g;
        rd++;
      } else {
        l[i] = r[i] = 0;
      }
    }
    Atomics.store(this.ctl, 6, rd);
    return true;
  }
}
registerProcessor('ring-player', RingPlayer);
