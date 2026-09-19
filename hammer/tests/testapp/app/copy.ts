// Every word a person reads, in every locale the server declares.
//
// The table is `Record`, never `Partial<Record>`, and that is the whole of its
// value: an error code or a validation reason added server-side is a compile
// error here until somebody writes the words for it. The alternative is a
// fallback string, and a fallback string is how `BAD_FORMAT` reaches a reader in
// an interface that is otherwise entirely in Arabic.
//
// `NOT_FOUND` says that the thing was not found and nothing else. anvil answers a
// denied request on a stealth route with a byte-identical 404 so that a probe
// cannot distinguish a missing object from a forbidden one; a client that renders
// "you do not have permission" there hands back the oracle the server spent a
// whole design removing.

import type { Copy } from "hammer";

import type { ErrorCode, Locale, ValidationReason } from "../api/hammer.generated.js";

export const copy: Copy<Locale, ErrorCode, ValidationReason> = {
    en: {
        errors: {
            UNAUTHENTICATED: "Please sign in to continue.",
            FORBIDDEN: "You do not have access to that.",
            NOT_FOUND: "We could not find that.",
            CAPABILITY_REQUIRED: "Confirm this action to continue.",
            CAPABILITY_INVALID: "That confirmation has expired. Start again.",
            VALIDATION_FAILED: "Check the highlighted fields.",
            CONFLICT: "That has already been done.",
            VERSION_MISMATCH: "Someone else changed this. Review the latest version.",
            RATE_LIMITED: "Too many attempts. Try again shortly.",
            PAYLOAD_TOO_LARGE: "That file is too large.",
            UNSUPPORTED_MEDIA: "That file type is not accepted.",
            SERVICE_UNAVAILABLE: "The service is busy. Try again in a moment.",
            INTERNAL: "Something went wrong on our side.",
            INSUFFICIENT_STORAGE: "There is no room to store that.",
            Unknown: "Something went wrong.",
        },
        reasons: {
            REQUIRED: "This is required.",
            TOO_SHORT: "That is too short.",
            TOO_LONG: "That is too long.",
            BAD_FORMAT: "Check the format.",
            BAD_CHARSET: "Some of those characters are not allowed.",
            OUT_OF_RANGE: "That is outside the allowed range.",
            NOT_ALLOWED: "That value is not allowed.",
            BAD_CHECKSUM: "Check the digits.",
            WEAK: "Choose something harder to guess.",
            BREACHED: "That password has appeared in a breach.",
            Unknown: "That value was not accepted.",
        },
    },
    ar: {
        errors: {
            UNAUTHENTICATED: "يرجى تسجيل الدخول للمتابعة.",
            FORBIDDEN: "ليس لديك صلاحية الوصول إلى ذلك.",
            NOT_FOUND: "تعذّر العثور على ذلك.",
            CAPABILITY_REQUIRED: "أكّد هذا الإجراء للمتابعة.",
            CAPABILITY_INVALID: "انتهت صلاحية التأكيد. ابدأ من جديد.",
            VALIDATION_FAILED: "راجع الحقول المحدّدة.",
            CONFLICT: "تم تنفيذ ذلك بالفعل.",
            VERSION_MISMATCH: "غيّر شخص آخر هذا العنصر. راجع أحدث نسخة.",
            RATE_LIMITED: "محاولات كثيرة. حاول بعد قليل.",
            PAYLOAD_TOO_LARGE: "هذا الملف كبير جدًا.",
            UNSUPPORTED_MEDIA: "نوع الملف غير مقبول.",
            SERVICE_UNAVAILABLE: "الخدمة مشغولة. حاول بعد لحظات.",
            INTERNAL: "حدث خطأ لدينا.",
            INSUFFICIENT_STORAGE: "لا توجد مساحة لتخزين ذلك.",
            Unknown: "حدث خطأ ما.",
        },
        reasons: {
            REQUIRED: "هذا الحقل مطلوب.",
            TOO_SHORT: "القيمة قصيرة جدًا.",
            TOO_LONG: "القيمة طويلة جدًا.",
            BAD_FORMAT: "تحقّق من الصيغة.",
            BAD_CHARSET: "بعض الأحرف غير مسموح بها.",
            OUT_OF_RANGE: "القيمة خارج النطاق المسموح.",
            NOT_ALLOWED: "هذه القيمة غير مسموح بها.",
            BAD_CHECKSUM: "تحقّق من الأرقام.",
            WEAK: "اختر قيمة أصعب في التخمين.",
            BREACHED: "ظهرت كلمة المرور هذه في تسريب.",
            Unknown: "لم يتم قبول هذه القيمة.",
        },
    },
};
