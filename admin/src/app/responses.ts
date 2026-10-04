// What each route answers with. anvil validates request bodies through its
// binders and writes responses by hand, so the descriptor carries no response
// schemas; the shapes are declared here, against the handlers in src/app/*.cc,
// by augmenting the generated interface. A route not listed stays `unknown`.

export type ImageRef = { readonly id: string; readonly src: string };

export type Me = {
    readonly id: string;
    readonly email: string;
    readonly type: "superadmin" | "full_control" | "staff" | "client";
    readonly role: string;
    readonly team: string;
    readonly permissions: readonly string[];
};

export type Application = {
    readonly id: string;
    readonly version: number;
    readonly created_at: number;
    readonly decision: string;
    readonly email: string;
    readonly first_name: string;
    readonly last_name: string;
    readonly phone: string;
    readonly reason: string;
    readonly referred_to: string;
    readonly status: ApplicationStatus;
    readonly team: string;
};

export type ApplicationStatus = "accepted" | "interview_scheduled" | "pending" | "referred" | "rejected";

export type Team = {
    readonly id: string;
    readonly name: string;
    readonly desc: string;
    readonly version: number;
    readonly members: number;
    readonly recruiting: boolean;
    readonly showcase: boolean;
};

export type TeamLead = { readonly id: string; readonly email: string; readonly role: string; readonly team: string };

export type Member = { readonly id: string; readonly name: string; readonly role: string; readonly version: number };

export type SectionValue = string | number | boolean;

export type StoredSection = {
    readonly key: string;
    readonly version: number;
    readonly data: Readonly<Record<string, SectionValue>>;
    readonly images: Readonly<Record<string, ImageRef | null>>;
};

export type GalleryItem = {
    readonly id: string;
    readonly version: number;
    readonly caption: string;
    readonly image: ImageRef | null;
};

export type FormStatus = "draft" | "active" | "closed";

export type FormOption = { readonly value: string; readonly label: string };

export type FormField = {
    readonly fid: string;
    readonly label: string;
    readonly type: string;
    readonly optional: boolean;
    readonly max_cp: number;
    readonly options: readonly FormOption[];
};

export type FormDefinition = {
    readonly id: string;
    readonly version: number;
    readonly title: string;
    readonly status: FormStatus;
    readonly closes_at: number | null;
    readonly max_submissions?: number;
    readonly submission_count?: number;
    readonly fields: readonly FormField[];
};

export type FormSummary = {
    readonly id: string;
    readonly title: string;
    readonly status: FormStatus;
    readonly version: number;
    readonly submission_count: number;
    readonly created_at: number;
};

export type FormResponse = {
    readonly id: string;
    readonly submitted_at: number;
    readonly form_version: number;
    readonly answers: Readonly<Record<string, string | readonly string[] | number>>;
};

export type TimeCursor = { readonly after: string; readonly at: number };

export type StaffAccount = {
    readonly id: string;
    readonly email: string;
    readonly type: Me["type"];
    readonly status: "active" | "disabled" | "locked" | "pending";
    readonly role: string;
    readonly team: string;
    readonly version: number;
    readonly permissions: readonly string[];
};

export type AuditRow = {
    readonly id: string;
    readonly at: number;
    readonly action: string;
    readonly code: string;
    readonly succeeded: boolean;
    readonly repeats: number;
    readonly actor: { readonly id: string; readonly name: string } | null;
    readonly subject: string | null;
    readonly network: string;
};

export type Dashboard = {
    readonly visits: readonly { readonly at: number; readonly count: number; readonly sessions: number }[];
    readonly visits_total: number;
    readonly applications_total: number;
    readonly applications_by_status: Readonly<Record<string, number>>;
    readonly applications_by_team: Readonly<Record<string, number>>;
    readonly teams: readonly { readonly name: string; readonly members: number }[];
};

declare module "../api/hammer.generated" {
    interface RouteResponses {
        "identity.me": Me;
        "dashboard.get": Dashboard;
        "audit.list": { readonly rows: readonly AuditRow[]; readonly next: TimeCursor | null };
        "applications.list": { readonly applications: readonly Application[]; readonly next: string | null };
        "applications.update": { readonly version: number };
        "teams.list": { readonly teams: readonly Team[] };
        "teams.create": { readonly id: string };
        "teams.leads": { readonly leads: readonly TeamLead[] };
        "members.list": { readonly members: readonly Member[] };
        "members.add": { readonly id: string };
        "sections.list": { readonly sections: readonly StoredSection[] };
        "sections.publish": { readonly version: number };
        "gallery.list": { readonly items: readonly GalleryItem[] };
        "gallery.add": { readonly id: string };
        "media.upload": { readonly id: string; readonly width: number; readonly height: number };
        "forms.list": { readonly forms: readonly FormSummary[]; readonly next: string | null };
        "forms.public": FormDefinition;
        "forms.create": { readonly id: string };
        "forms.update": { readonly version: number };
        "forms.delete": { readonly responses_removed: number };
        "responses.list": {
            readonly form: FormDefinition;
            readonly responses: readonly FormResponse[];
            readonly next: TimeCursor | null;
        };
        "responses.export": { readonly filename: string; readonly csv: string };
        "staff.list": { readonly staff: readonly StaffAccount[] };
        "staff.create": { readonly id: string };
    }
}
