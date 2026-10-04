// Image uploads through anvil's media pipeline. The file streams from the
// `File` handle as the request body; it is sniffed, size-capped, re-encoded and
// stripped of metadata server-side, so what the public site serves is never the
// uploaded bytes.

import { api } from "./platform";
import type { CallResult } from "./ui";
import { kUploadMaxBytes, routeMediaUpload } from "../api/hammer.generated";

// The namespace accepts JPEG and PNG (anvil_app_config.h). Refused here before
// the first byte to save the round trip; the server is the enforcement.
export const kImageTypes = ["image/jpeg", "image/png"] as const;

// No progress callback: with one, hammer streams the body, and a browser sends
// a streamed request body only over HTTP/2. Without it the `File` itself is the
// body, which works over any transport and still never enters the JS heap.
export async function uploadImage(
    file: File,
    signal: AbortSignal,
): Promise<CallResult<{ readonly id: string; readonly width: number; readonly height: number }>> {
    return await api.upload(routeMediaUpload, { maxBytes: kUploadMaxBytes, accept: kImageTypes }, { file, signal });
}

// A served image URL for one of the four roles the namespace renders.
export function imageUrl(src: string, role: "thumb" | "card" | "hero" | "full" = "card"): string {
    return `${src}/${role}`;
}
