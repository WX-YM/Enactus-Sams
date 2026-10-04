// Unpadded base64url, for the binary values anvil's contracts carry as text:
// the prehash salt and credential, and an image edit's recipe.
//
// Hand-written rather than `atob`/`btoa`: those take the standard alphabet,
// throw on anything else, and traffic in binary strings — a second copy of the
// credential as a JavaScript string, which nothing can zero.

const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

function digit(code: number): number {
    if (code >= 65 && code <= 90) {
        return code - 65;
    }
    if (code >= 97 && code <= 122) {
        return code - 71;
    }
    if (code >= 48 && code <= 57) {
        return code + 4;
    }
    if (code === 45) {
        return 62;
    }
    if (code === 95) {
        return 63;
    }
    return -1;
}

export function encodeBase64Url(bytes: Uint8Array): string {
    let out = "";
    let i = 0;
    for (; i + 2 < bytes.length; i += 3) {
        const group = ((bytes[i] ?? 0) << 16) | ((bytes[i + 1] ?? 0) << 8) | (bytes[i + 2] ?? 0);
        out +=
            kAlphabet.charAt((group >>> 18) & 63) +
            kAlphabet.charAt((group >>> 12) & 63) +
            kAlphabet.charAt((group >>> 6) & 63) +
            kAlphabet.charAt(group & 63);
    }
    const tail = bytes.length - i;
    if (tail === 1) {
        const group = (bytes[i] ?? 0) << 16;
        out += kAlphabet.charAt((group >>> 18) & 63) + kAlphabet.charAt((group >>> 12) & 63);
    } else if (tail === 2) {
        const group = ((bytes[i] ?? 0) << 16) | ((bytes[i + 1] ?? 0) << 8);
        out +=
            kAlphabet.charAt((group >>> 18) & 63) +
            kAlphabet.charAt((group >>> 12) & 63) +
            kAlphabet.charAt((group >>> 6) & 63);
    }
    return out;
}

// Exactly `length` bytes from exactly the unpadded text that encodes them, or
// null. Non-canonical trailing bits are refused: two spellings of one salt is a
// server bug worth hearing about, not one to paper over.
export function decodeBase64Url(text: string, length: number): Uint8Array | null {
    if (text.length !== Math.ceil((length * 4) / 3)) {
        return null;
    }
    const out = new Uint8Array(length);
    let accumulator = 0;
    let bits = 0;
    let written = 0;
    for (let i = 0; i < text.length; i += 1) {
        const value = digit(text.charCodeAt(i));
        if (value < 0) {
            return null;
        }
        accumulator = ((accumulator << 6) | value) & 0xffffff;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[written] = (accumulator >>> bits) & 0xff;
            written += 1;
        }
    }
    if (written !== length || (accumulator & ((1 << bits) - 1)) !== 0) {
        return null;
    }
    return out;
}
