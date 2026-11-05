// heatshrink with an 8 bit window and a 4 bit lookahead, the parameters the printer
// and the module both decode. A flag bit, then either 8 bits of literal or an 8 bit
// distance and a 4 bit length, each stored minus one, most significant bit first.

const WINDOW = 256;
const LOOKAHEAD = 16;

export function heatshrinkEncode(input) {
  const out = new Uint8Array(Math.ceil(input.length * 9 / 8) + 4);
  let pos = 0, acc = 0, bits = 0;
  const put = (value, count) => {
    acc = (acc << count) | (value & ((1 << count) - 1));
    bits += count;
    while (bits >= 8) {
      bits -= 8;
      out[pos++] = (acc >>> bits) & 255;
    }
    acc &= (1 << bits) - 1;
  };

  // Where each recent byte value was last seen, chained back through the window
  const last = new Int32Array(256).fill(-1);
  const prev = new Int32Array(input.length);

  for (let i = 0; i < input.length; ) {
    let bestLen = 0, bestDist = 0;
    const limit = Math.min(LOOKAHEAD, input.length - i);
    for (let j = last[input[i]]; j >= 0 && i - j <= WINDOW; j = prev[j]) {
      let n = 1;
      while (n < limit && input[j + n] === input[i + n]) n++;
      if (n > bestLen) {
        bestLen = n;
        bestDist = i - j;
        if (n === limit) break;
      }
    }
    const take = bestLen >= 2 ? bestLen : 1;
    if (bestLen >= 2) {
      put(0, 1);
      put(bestDist - 1, 8);
      put(bestLen - 1, 4);
    } else {
      put(1, 1);
      put(input[i], 8);
    }
    for (let k = 0; k < take; k++, i++) {
      prev[i] = last[input[i]];
      last[input[i]] = i;
    }
  }
  if (bits) put(0, 8 - bits);
  return out.subarray(0, pos);
}
