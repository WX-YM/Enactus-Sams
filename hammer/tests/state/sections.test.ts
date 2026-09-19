// A section's published document and its draft, which must never share a key.

import { describe, expect, it } from "vitest";

import { ResourceStore } from "../../src/state/resource.js";
import { Sections, labelAt } from "../../src/state/sections.js";
import { routeIdentityMe } from "../testapp/api/hammer.generated.js";
import type { Api } from "../testapp/app/client.js";
import { FakeServer } from "../support/fake_fetch.js";
import { clientHarness, settle } from "../support/state.js";

function sections(server: FakeServer) {
    const wire = clientHarness({ server });
    const resources = new ResourceStore<Api>({
        client: wire.client,
        cache: { classes: { section: 8 }, defaultMaxEntries: 8 },
        now: wire.now,
    });
    const held = new Sections<Api, typeof routeIdentityMe>({
        resources,
        route: routeIdentityMe,
        // The query names are the server's vocabulary, so the application
        // shapes it and hammer names nothing.
        query: ({ key, stage }) => ({ section: key, stage }),
        class: "section",
    });
    return { sections: held, resources, server, wire };
}

const kCacheable = { headers: { "Cache-Control": "max-age=600" } };

describe("Sections", () => {
    it("reads the published document and the draft as two requests", async () => {
        const server = new FakeServer()
            .reply({ body: { title: "live" }, ...kCacheable })
            .always({ body: { title: "editing" }, ...kCacheable });
        const { sections: held } = sections(server);

        const published = held.open("home.about", "published");
        await settle();
        const draft = held.open("home.about", "draft");
        await settle();

        expect(server.calls).toBe(2);
        expect(published.state.get().value).toEqual({ title: "live" });
        expect(draft.state.get().value).toEqual({ title: "editing" });

        published.release();
        draft.release();
    });

    it("never serves a draft from the published key", async () => {
        // A draft rendered to every visitor, from one editor's preview, with no
        // request having gone wrong anywhere.
        const server = new FakeServer()
            .reply({ body: { title: "editing" }, ...kCacheable })
            .always({ body: { title: "live" }, ...kCacheable });
        const { sections: held } = sections(server);

        const draft = held.open("home.about", "draft");
        await settle();
        draft.release();

        const published = held.open("home.about", "published");
        await settle();

        expect(published.state.get().value).toEqual({ title: "live" });
        expect(published.key()).not.toBe(draft.key());
        published.release();
    });

    it("keys each section separately", async () => {
        const server = new FakeServer().always({ body: { title: "x" }, ...kCacheable });
        const { sections: held } = sections(server);

        const about = held.open("home.about", "published");
        const hero = held.open("home.hero", "published");
        await settle();

        expect(about.key()).not.toBe(hero.key());
        expect(server.calls).toBe(2);
        about.release();
        hero.release();
    });

    it("drops both entries when a draft is published", async () => {
        const server = new FakeServer().always({ body: { title: "x" }, ...kCacheable });
        const { sections: held } = sections(server);

        const published = held.open("home.about", "published");
        await settle();
        published.release();
        const draft = held.open("home.about", "draft");
        await settle();
        draft.release();
        expect(server.calls).toBe(2);

        held.published("home.about");
        await settle();

        // Dropping only the published entry is how an editor keeps seeing the
        // draft they just published as though it were still pending.
        const again = held.open("home.about", "draft");
        expect(again.state.get().status).toBe("loading");
        again.release();
    });
});

describe("labelAt", () => {
    it("reads by the locale table's index, which the client never renumbers", () => {
        expect(labelAt(["Title", "العنوان"], 0)).toBe("Title");
        expect(labelAt(["Title", "العنوان"], 1)).toBe("العنوان");
    });

    it("answers null rather than substituting another locale's words", () => {
        // A short array is a generation failure; what this must not do is
        // silently render a translation nobody wrote.
        expect(labelAt(["Title"], 1)).toBeNull();
        expect(labelAt(["Title"], -1)).toBeNull();
        expect(labelAt(["Title"], 1.5)).toBeNull();
    });
});
