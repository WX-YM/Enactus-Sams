// A PNG reader for tests: 8-bit RGB or RGBA, not interlaced — what anvil's
// PNG masters and Chromium's screenshots both are, and nothing else.
//
// Written here rather than pulled in because a pixel comparison is the only
// thing in the repository that needs one, and a dev dependency is a design
// decision with a policy entry, not a convenience (`CLAUDE.md` §12). The
// platform has the hard half: `node:zlib` inflates, and what is left is the
// chunk walk and the five scanline filters of RFC 2083 §6.

import { inflateSync } from "node:zlib";

export type Pixels = {
    readonly widthPx: number;
    readonly heightPx: number;
    // RGB, three bytes a pixel, row-major. An alpha channel is dropped: both
    // sides of every comparison here are opaque.
    readonly rgb: Uint8Array;
};

const kSignature = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

function paeth(a: number, b: number, c: number): number {
    const p = a + b - c;
    const pa = Math.abs(p - a);
    const pb = Math.abs(p - b);
    const pc = Math.abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

export function decodePng(bytes: Uint8Array): Pixels {
    for (let i = 0; i < kSignature.length; i += 1) {
        if (bytes[i] !== kSignature[i]) {
            throw new Error("not a PNG");
        }
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let at = kSignature.length;
    let widthPx = 0;
    let heightPx = 0;
    let channels = 0;
    const data: Uint8Array[] = [];
    while (at + 8 <= bytes.length) {
        const length = view.getUint32(at);
        const type = String.fromCharCode(...bytes.subarray(at + 4, at + 8));
        const body = bytes.subarray(at + 8, at + 8 + length);
        if (type === "IHDR") {
            widthPx = view.getUint32(at + 8);
            heightPx = view.getUint32(at + 12);
            const depth = body[8];
            const colour = body[9];
            const interlace = body[12];
            if (depth !== 8 || (colour !== 2 && colour !== 6) || interlace !== 0) {
                throw new Error(`unsupported PNG: depth ${depth}, colour ${colour}, interlace ${interlace}`);
            }
            channels = colour === 6 ? 4 : 3;
        } else if (type === "IDAT") {
            data.push(body);
        } else if (type === "IEND") {
            break;
        }
        at += 12 + length;
    }
    if (channels === 0) {
        throw new Error("PNG has no IHDR");
    }

    const raw = inflateSync(Buffer.concat(data));
    const stride = widthPx * channels;
    if (raw.length !== (stride + 1) * heightPx) {
        throw new Error("PNG data is not the size its header declares");
    }
    const out = new Uint8Array(stride * heightPx);
    for (let y = 0; y < heightPx; y += 1) {
        const filter = raw[y * (stride + 1)];
        const from = y * (stride + 1) + 1;
        const row = y * stride;
        const above = row - stride;
        for (let x = 0; x < stride; x += 1) {
            const value = raw[from + x] as number;
            const a = x >= channels ? (out[row + x - channels] as number) : 0;
            const b = y > 0 ? (out[above + x] as number) : 0;
            const c = x >= channels && y > 0 ? (out[above + x - channels] as number) : 0;
            let decoded: number;
            switch (filter) {
                case 0: decoded = value; break;
                case 1: decoded = value + a; break;
                case 2: decoded = value + b; break;
                case 3: decoded = value + ((a + b) >> 1); break;
                case 4: decoded = value + paeth(a, b, c); break;
                default: throw new Error(`unknown PNG filter ${filter}`);
            }
            out[row + x] = decoded & 0xff;
        }
    }

    if (channels === 3) {
        return { widthPx, heightPx, rgb: out };
    }
    const rgb = new Uint8Array(widthPx * heightPx * 3);
    for (let i = 0, j = 0; i < out.length; i += 4, j += 3) {
        rgb[j] = out[i] as number;
        rgb[j + 1] = out[i + 1] as number;
        rgb[j + 2] = out[i + 2] as number;
    }
    return { widthPx, heightPx, rgb };
}
