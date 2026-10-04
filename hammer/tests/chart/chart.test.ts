//
// The chart, and the ordering that makes its accessibility mechanical: the
// drawing is hidden and the table is the reading, so there is no arrangement of
// this component where a focusable node ends up under an `aria-hidden` one.

import { describe, expect, it } from "../support/test.js";

import type { ClassNames } from "../../src/core/tables.js";
import { renderChart } from "../../src/chart/chart.js";
import type { ChartCopy, ChartPart, ChartSeries } from "../../src/chart/chart.js";

const kClasses: ClassNames<ChartPart> = {
    root: "k",
    figure: "k-figure",
    series: "k-series",
    point: "k-point",
    hit: "k-hit",
    tick: "k-tick",
    table: "k-table",
    row: "k-row",
    current: "k-current",
};

const kCopy: ChartCopy = {
    caption: "Signups per week",
    seriesHeader: "Series",
    xHeader: "Week",
    yHeader: "Signups",
};

const kSeries: readonly ChartSeries[] = [
    { key: "new", label: "New", points: [{ x: 0, y: 10 }, { x: 1, y: 30 }, { x: 2, y: 20 }] },
];

function mount(over: Partial<Parameters<typeof renderChart>[1]> = {}) {
    const host = document.createElement("div");
    document.body.append(host);
    const selected: { series: string; y: number }[] = [];
    const view = renderChart(host, {
        series: kSeries,
        widthCssPx: 200,
        heightCssPx: 100,
        formatX: (v) => `w${v}`,
        formatY: (v) => String(v),
        classes: kClasses,
        copy: kCopy,
        onSelect: (series, point) => selected.push({ series: series.key, y: point.y }),
        ...over,
    });
    const rows = (): HTMLTableRowElement[] => Array.from(host.querySelectorAll("tbody tr"));
    const press = (key: string): void => {
        host.querySelector("table")?.dispatchEvent(
            new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true }),
        );
    };
    return { host, view, selected, rows, press, close: () => { view.close(); host.remove(); } };
}

describe("the drawing", () => {
    it("is an SVG in the SVG namespace, not an unknown HTML element", () => {
        const app = mount();
        const figure = app.host.querySelector(".k-figure");
        expect(figure?.namespaceURI).toBe("http://www.w3.org/2000/svg");
        expect(app.host.querySelector("path")?.namespaceURI).toBe("http://www.w3.org/2000/svg");
        app.close();
    });

    // It says nothing the table does not, and two readings of one figure one
    // after the other is worse than one.
    it("is hidden from assistive technology", () => {
        const app = mount();
        expect(app.host.querySelector(".k-figure")?.getAttribute("aria-hidden")).toBe("true");
        app.close();
    });

    // The rule this ordering makes mechanical rather than remembered.
    it("holds nothing focusable, because it is the hidden half", () => {
        const app = mount();
        const figure = app.host.querySelector(".k-figure");
        expect(figure?.querySelector("[tabindex]")).toBeNull();
        expect(figure?.getAttribute("focusable")).toBe("false");
        app.close();
    });

    it("draws a path through the points", () => {
        const app = mount();
        const d = app.host.querySelector("path")?.getAttribute("d") ?? "";
        expect(d.startsWith("M")).toBe(true);
        expect(d).toContain("L");
        app.close();
    });

    // This one line is the difference between a chart and its reflection: a
    // screen's y grows downward and a value does not.
    it("puts the largest value nearest the top", () => {
        const app = mount();
        const circles = Array.from(app.host.querySelectorAll("circle"));
        const ys = circles.map((c) => Number(c.getAttribute("cy")));
        // The middle point is the largest value, so it must have the smallest y.
        expect(ys[1]).toBeLessThan(ys[0] ?? 0);
        expect(ys[1]).toBeLessThan(ys[2] ?? 0);
        app.close();
    });

    // A four pixel target is a target most people miss on a phone.
    it("gives each point a hit region sized for a finger", () => {
        const app = mount();
        const hit = app.host.querySelector(".k-hit");
        expect(Number(hit?.getAttribute("width"))).toBeGreaterThanOrEqual(24);
        expect(Number(hit?.getAttribute("height"))).toBeGreaterThanOrEqual(24);
        app.close();
    });

    // A colour is the application's, and an inline one would be blocked by the
    // CSP the application serves anyway.
    it("carries no colour of its own", () => {
        const app = mount();
        const line = app.host.querySelector("path");
        expect(line?.getAttribute("stroke")).toBeNull();
        expect(line?.getAttribute("fill")).toBeNull();
        expect(line?.getAttribute("style")).toBeNull();
        app.close();
    });

    it("draws a series that has not moved without failing", () => {
        const app = mount({
            series: [{ key: "flat", label: "Flat", points: [{ x: 0, y: 5 }, { x: 1, y: 5 }] }],
        });
        const d = app.host.querySelector("path")?.getAttribute("d") ?? "";
        expect(d).not.toContain("NaN");
        app.close();
    });

    it("draws nothing rather than failing on an empty series", () => {
        const app = mount({ series: [{ key: "none", label: "None", points: [] }] });
        expect(app.host.querySelector("path")?.getAttribute("d")).toBe("");
        app.close();
    });
});

// A chart that exists only as pixels is a chart part of the audience cannot
// read.
describe("the reading", () => {
    it("is a real table with a caption and headers", () => {
        const app = mount();
        expect(app.host.querySelector("caption")?.textContent).toBe("Signups per week");
        const headers = Array.from(app.host.querySelectorAll("thead th")).map((c) => c.textContent);
        expect(headers).toEqual(["Series", "Week", "Signups"]);
        for (const cell of app.host.querySelectorAll("thead th")) {
            expect(cell.getAttribute("scope")).toBe("col");
        }
        app.close();
    });

    it("has a row per point, named by its series", () => {
        const app = mount();
        const rows = app.rows();
        expect(rows.length).toBe(3);
        expect(rows[0]?.querySelector("th")?.getAttribute("scope")).toBe("row");
        expect(rows[0]?.querySelector("th")?.textContent).toBe("New");
        app.close();
    });

    // A chart that formatted its own numbers would ship one locale's
    // punctuation to every consumer.
    it("renders every number through the application's formatter", () => {
        const app = mount();
        const cells = Array.from(app.rows()[0]?.querySelectorAll("td") ?? []);
        expect(cells.map((c) => c.textContent)).toEqual(["w0", "10"]);
        app.close();
    });

    it("names the figure for assistive technology", () => {
        const app = mount();
        const root = app.host.querySelector(".k");
        expect(root?.getAttribute("role")).toBe("group");
        expect(root?.getAttribute("aria-label")).toContain("Signups per week");
        app.close();
    });
});

describe("walking the series", () => {
    it("is one tab stop, never a positive tabindex", () => {
        const app = mount();
        const stops = app.rows().filter((r) => r.tabIndex === 0);
        expect(stops.length).toBe(1);
        for (const row of app.rows()) {
            expect(row.tabIndex).toBeLessThanOrEqual(0);
        }
        app.close();
    });

    it("moves with the arrows, in both axes", () => {
        const app = mount();
        app.press("ArrowRight");
        expect(document.activeElement).toBe(app.rows()[1]);
        app.press("ArrowDown");
        expect(document.activeElement).toBe(app.rows()[2]);
        app.press("ArrowLeft");
        expect(document.activeElement).toBe(app.rows()[1]);
        app.close();
    });

    it("reaches both ends and stops there", () => {
        const app = mount();
        app.press("End");
        expect(document.activeElement).toBe(app.rows()[2]);
        app.press("ArrowRight");
        expect(document.activeElement).toBe(app.rows()[2]);
        app.press("Home");
        expect(document.activeElement).toBe(app.rows()[0]);
        app.close();
    });

    // The page would otherwise scroll under the figure somebody is reading.
    it("does not let the page scroll under it", () => {
        const app = mount();
        const event = new KeyboardEvent("keydown", { key: "ArrowRight", bubbles: true, cancelable: true });
        app.host.querySelector("table")?.dispatchEvent(event);
        expect(event.defaultPrevented).toBe(true);
        app.close();
    });

    it("reports what the reader moved onto", () => {
        const app = mount();
        app.press("ArrowRight");
        expect(app.selected).toEqual([{ series: "new", y: 30 }]);
        app.close();
    });

    it("marks where the reader is", () => {
        const app = mount();
        app.press("ArrowRight");
        expect(app.rows()[1]?.className).toContain("k-current");
        expect(app.rows()[0]?.className).not.toContain("k-current");
        app.close();
    });
});

describe("closing", () => {
    it("takes the figure out of the document", () => {
        const app = mount();
        app.view.close();
        expect(app.host.querySelector(".k")).toBeNull();
        app.host.remove();
    });
});
