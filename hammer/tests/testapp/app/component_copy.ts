// Every word hammer's own components put on a screen, in every locale the
// server declares.
//
// Separate from `copy.ts`, and that is not tidying. `Copy<Locale, Code, Reason>`
// is the FAILURE vocabulary — the words for an error code and for a validation
// reason — and it is total over two unions the descriptor generates. A
// component's own words are total over a union the COMPONENT declares, which is
// a different seam with a different source of truth, and folding them into one
// record would make an addition to either look like an addition to the other.
//
// Both are §13's rule; this is it at component granularity
// (`docs/01-seams.md` §19).
//
// The slots that carry a number are functions, and every one of them is a place
// where a plural rule and a digit shape live. Arabic has six plural categories
// and this table is where that fact belongs — a library formatting the count
// itself would be shipping English grammar to every consumer at once.

import type { ChartCopy } from "hammer/chart";
import type { EditorCopy } from "hammer/edit";
import type {
    BellCopy,
    ConsentCopy,
    ErrorCopy,
    FormCopy,
    InboxCopy,
    PagerCopy,
    UploadCopy,
} from "hammer/dom";

import type { ErrorCode, Locale } from "../api/hammer.generated.js";

export type ComponentCopy = {
    readonly form: FormCopy;
    readonly bell: BellCopy;
    readonly inbox: InboxCopy;
    readonly upload: UploadCopy;
    readonly pager: PagerCopy;
    readonly consent: ConsentCopy;
    readonly chart: ChartCopy;
    readonly editor: EditorCopy;
    readonly failure: ErrorCopy<ErrorCode | "Unknown">;
};

const en: ComponentCopy = {
    form: {
        required: "required",
        remaining: (codePoints) => `${codePoints} characters left`,
        summary: "Problems with this form",
        submit: "Save",
    },
    bell: {
        label: "Notifications",
        unread: (count) => (count === 1 ? "1 unread notification" : `${count} unread notifications`),
        arrived: (count) => (count === 1 ? "1 new notification" : `${count} new notifications`),
        empty: "Nothing yet.",
        markRead: "Mark all as read",
    },
    inbox: {
        label: "Notifications",
        arrived: (count) => (count === 1 ? "1 new notification" : `${count} new notifications`),
        empty: "Nothing yet.",
    },
    upload: {
        label: "Choose a file",
        drop: "Or drop one here",
        cancel: "Cancel",
        progress: (sentBytes, totalBytes) => `${sentBytes} of ${totalBytes} bytes sent`,
        refused: {
            "too-large": "That file is too large.",
            "unsupported-media": "That file type is not accepted.",
            "bad-parameter": "That does not look like a file.",
        },
    },
    pager: {
        more: "Show more",
        loading: "Loading more",
        failed: "That did not load.",
        retry: "Try again",
    },
    consent: {
        question: "May we measure how this site is used?",
        grant: "Yes",
        deny: "No",
        granted: "Measurement is on.",
        denied: "Measurement is off.",
        revoke: "Turn it off",
        reconsider: "Turn it on",
    },
    chart: {
        caption: "Signups per week",
        seriesHeader: "Series",
        xHeader: "Week",
        yHeader: "Signups",
    },
    editor: {
        canvas: "Photo being edited",
        toolbar: "Editing tools",
        crop: "Crop",
        draw: "Draw",
        rotateLeft: "Rotate left",
        rotateRight: "Rotate right",
        flip: "Flip",
        undo: "Undo",
        redo: "Redo",
        resetCrop: "Remove crop",
        cropBox: "Crop area, arrow keys move it",
        handles: {
            n: "Top edge", s: "Bottom edge", e: "Right edge", w: "Left edge",
            ne: "Top right corner", nw: "Top left corner", se: "Bottom right corner", sw: "Bottom left corner",
        },
        strokes: "Drawn lines",
        strokeName: (index, total) => `Line ${index} of ${total}`,
        deleteStroke: "Delete line",
        resize: "Longest side, in pixels",
        cropSize: (width, height) => `${width} by ${height} pixels`,
        refused: {
            "edit.format": "That edit could not be read.",
            "edit.canonical": "That edit could not be read.",
            "edit.empty": "Nothing has been changed yet.",
            "edit.crop": "The crop is outside the photo.",
            "edit.too_small": "That would be too small to show.",
            "edit.upscale": "An image can only be made smaller.",
            "edit.stroke": "That line cannot be drawn.",
            "edit.bounds": "No more lines fit on this photo.",
            "stroke-limit": "That line is too long. Try a shorter one.",
        },
    },
    failure: {
        // Shares the failure table rather than restating it: the words for a
        // code are already answered once, and a second copy is a second thing to
        // keep in step with the server's enum.
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
        unreachable: "We could not reach the server.",
        requestId: "Reference",
    },
};

const ar: ComponentCopy = {
    form: {
        required: "مطلوب",
        remaining: (codePoints) => `بقي ${codePoints} حرفًا`,
        summary: "مشكلات في هذا النموذج",
        submit: "حفظ",
    },
    bell: {
        label: "الإشعارات",
        unread: (count) => `${count} إشعارًا غير مقروء`,
        arrived: (count) => `${count} إشعارًا جديدًا`,
        empty: "لا شيء بعد.",
        markRead: "تعليم الكل كمقروء",
    },
    inbox: {
        label: "الإشعارات",
        arrived: (count) => `${count} إشعارًا جديدًا`,
        empty: "لا شيء بعد.",
    },
    upload: {
        label: "اختر ملفًا",
        drop: "أو أسقطه هنا",
        cancel: "إلغاء",
        progress: (sentBytes, totalBytes) => `أُرسل ${sentBytes} من ${totalBytes} بايت`,
        refused: {
            "too-large": "هذا الملف كبير جدًا.",
            "unsupported-media": "نوع الملف غير مقبول.",
            "bad-parameter": "هذا لا يبدو ملفًا.",
        },
    },
    pager: {
        more: "عرض المزيد",
        loading: "جارٍ التحميل",
        failed: "تعذّر التحميل.",
        retry: "أعد المحاولة",
    },
    consent: {
        question: "هل تسمح لنا بقياس استخدام الموقع؟",
        grant: "نعم",
        deny: "لا",
        granted: "القياس مفعّل.",
        denied: "القياس معطّل.",
        revoke: "أوقفه",
        reconsider: "شغّله",
    },
    chart: {
        caption: "التسجيلات في الأسبوع",
        seriesHeader: "السلسلة",
        xHeader: "الأسبوع",
        yHeader: "التسجيلات",
    },
    editor: {
        canvas: "الصورة قيد التعديل",
        toolbar: "أدوات التعديل",
        crop: "قص",
        draw: "رسم",
        rotateLeft: "تدوير لليسار",
        rotateRight: "تدوير لليمين",
        flip: "قلب",
        undo: "تراجع",
        redo: "إعادة",
        resetCrop: "إزالة القص",
        cropBox: "منطقة القص، تتحرك بمفاتيح الأسهم",
        handles: {
            n: "الحافة العليا", s: "الحافة السفلى", e: "الحافة اليمنى", w: "الحافة اليسرى",
            ne: "الزاوية العليا اليمنى", nw: "الزاوية العليا اليسرى", se: "الزاوية السفلى اليمنى", sw: "الزاوية السفلى اليسرى",
        },
        strokes: "الخطوط المرسومة",
        strokeName: (index, total) => `الخط ${index} من ${total}`,
        deleteStroke: "حذف الخط",
        resize: "أطول ضلع، بالبكسل",
        cropSize: (width, height) => `${width} × ${height} بكسل`,
        refused: {
            "edit.format": "تعذّرت قراءة هذا التعديل.",
            "edit.canonical": "تعذّرت قراءة هذا التعديل.",
            "edit.empty": "لم يتغيّر شيء بعد.",
            "edit.crop": "منطقة القص خارج الصورة.",
            "edit.too_small": "ستكون الصورة أصغر من أن تُعرض.",
            "edit.upscale": "يمكن تصغير الصورة فقط.",
            "edit.stroke": "لا يمكن رسم هذا الخط.",
            "edit.bounds": "لا تتسع الصورة لخطوط أخرى.",
            "stroke-limit": "هذا الخط طويل جدًا. جرّب خطًا أقصر.",
        },
    },
    failure: {
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
        unreachable: "تعذّر الوصول إلى الخادم.",
        requestId: "المرجع",
    },
};

// Total over the locale union, like every other table here: a locale the server
// appends is a compile error until somebody writes the words for it.
export const componentCopy: Readonly<Record<Locale, ComponentCopy>> = { en, ar };
