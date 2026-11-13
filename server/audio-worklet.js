// Accumulate 1 second of mono PCM. AudioContext runs at 16 kHz.
class CameraMic extends AudioWorkletProcessor {
  constructor() { super(); this.samples = new Int16Array(16000); this.offset = 0; }
  process(inputs, outputs) {
    // Never monitor the microphone through the user's local speaker.
    for (const channel of outputs[0] || []) channel.fill(0);
    const input = inputs[0]?.[0];
    if (input) for (const sample of input) {
      this.samples[this.offset++] = Math.round(Math.max(-1, Math.min(1, sample)) * 32767);
      if (this.offset === 16000) {
        const bytes = new ArrayBuffer(32000), view = new DataView(bytes);
        for (let i = 0; i < 16000; ++i) view.setInt16(i * 2, this.samples[i], true);
        this.port.postMessage(bytes, [bytes]); this.offset = 0;
      }
    }
    return true;
  }
}
registerProcessor('camera-mic', CameraMic);
