// A chart: geometry, hit regions, a keyboard path, and the data table that is
// the actual accessible representation of the figure.
//
// --- the table is not a fallback, it is the reading -------------------------
//
// A chart that exists only as pixels is a chart part of the audience cannot
// read. So the table is always in the document and is always in the
// accessibility tree; the drawing is marked `aria-hidden` because it says
// nothing the table does not, and two descriptions of one figure read out one
// after the other is worse than one.
//
// That ordering is also what keeps the rule in `CLAUDE.md` §9 — no `aria-hidden`
// over anything focusable — mechanical rather than remembered: the drawing is
// hidden, so nothing in it may be focusable, so the keyboard path is the table's
// rows. There is no arrangement of this component where a focusable node ends up
// under a hidden one.
//
// --- no colour, no format, no label -----------------------------------------
//
// Scales, ticks, path strings and hit regions are arithmetic. Which series is
// which colour, how a number reads in this locale, and what the axes are called
// are the application's (`CLAUDE.md` §9). Every one of them arrives as a
// parameter and none of them has a default here.

import type { ClassNames } from "../core/tables.js";

import { Closers, detach, documentOf, elementIn, isolatedAttribute, setUserText, svgIn } from "./mount.js";
import type { Mounted } from "./mount.js";
import { extent, linear, ticks } from "./scale.js";
import type { Interval } from "./scale.js";

export type ChartPart =
    | "root"
    | "figure"
    | "series"
    | "point"
    | "hit"
    | "tick"
    | "table"
    | "row"
    | "current";

export type ChartCopy = {
    // Names the figure. A chart with no accessible name is an unlabelled region,
    // which is the single most common accessibility defect there is.
    readonly caption: string;

    // Column headings for the table that carries the data.
    readonly seriesHeader: string;
    readonly xHeader: string;
    readonly yHeader: string;
};

export type ChartPoint = {
    readonly x: number;
    readonly y: number;
};

export type ChartSeries = {
    readonly key: string;

    // The words for this series. The application's, in the locale it is reading.
    readonly label: string;

    readonly points: readonly ChartPoint[];
};

export type ChartOptions = {
    readonly series: readonly ChartSeries[];

    // The box the figure is drawn in, in CSS pixels, and the name says so.
    readonly widthCssPx: number;
    readonly heightCssPx: number;

    // How a number reads. `Intl.NumberFormat`, a date, a duration, a currency —
    // all of them are the application's, and a chart that formatted its own axis
    // would be shipping one locale's punctuation to every consumer.
    readonly formatX: (value: number) => string;
    readonly formatY: (value: number) => string;

    // How many ticks to aim for. A hint: the step is chosen from round numbers,
    // so the count that comes back is near this rather than equal to it.
    readonly tickCount?: number;

    // Called as the reader moves through the data, by pointer or by keyboard.
    readonly onSelect?: (series: ChartSeries, point: ChartPoint) => void;

    readonly classes: ClassNames<ChartPart>;
    readonly copy: ChartCopy;
};

// Room for the tick labels. Not a style: it is the arithmetic that stops the
// leftmost label being drawn outside the box, and the application cannot supply
// it because it does not know how wide its own formatted numbers are either.
const kInsetCssPx = 4;

function pathOf(points: readonly ChartPoint[], toX: (v: number) => number, toY: (v: number) => number): string {
    // Built in one pass with no intermediate arrays: a series of ten thousand
    // points would otherwise allocate three of them to produce one string.
    let out = "";
    for (const point of points) {
        if (!Number.isFinite(point.x) || !Number.isFinite(point.y)) {
            continue;
        }
        out += `${out.length === 0 ? "M" : "L"}${toX(point.x)} ${toY(point.y)}`;
    }
    return out;
}

export function renderChart(mount: Element, options: ChartOptions): Mounted {
    const doc = documentOf(mount);
    const closers = new Closers();
    const { classes, copy, series } = options;

    const root = elementIn(doc, "div", classes.root);

    const xs: number[] = [];
    const ys: number[] = [];
    for (const one of series) {
        for (const point of one.points) {
            xs.push(point.x);
            ys.push(point.y);
        }
    }

    const kEmpty: Interval = { from: 0, to: 1 };
    const domainX = extent(xs) ?? kEmpty;
    const domainY = extent(ys) ?? kEmpty;

    const toX = linear(domainX, { from: kInsetCssPx, to: options.widthCssPx - kInsetCssPx });
    // Inverted, because a screen's y grows downward and a value does not. This
    // one line is the difference between a chart and its reflection.
    const toY = linear(domainY, { from: options.heightCssPx - kInsetCssPx, to: kInsetCssPx });

    const figure = svgIn(doc, "svg", classes.figure);
    figure.setAttribute("viewBox", `0 0 ${options.widthCssPx} ${options.heightCssPx}`);
    figure.setAttribute("width", String(options.widthCssPx));
    figure.setAttribute("height", String(options.heightCssPx));
    // It says nothing the table does not. Two readings of one figure, one after
    // the other, is worse than one.
    figure.setAttribute("aria-hidden", "true");
    // Nothing in here is focusable, and this is what keeps it that way when
    // somebody adds a mark later.
    figure.setAttribute("focusable", "false");

    for (const value of ticks(domainY, options.tickCount ?? 4)) {
        const line = svgIn(doc, "line", classes.tick);
        line.setAttribute("x1", String(kInsetCssPx));
        line.setAttribute("x2", String(options.widthCssPx - kInsetCssPx));
        line.setAttribute("y1", String(toY(value)));
        line.setAttribute("y2", String(toY(value)));
        figure.append(line);
    }

    for (const one of series) {
        const line = svgIn(doc, "path", classes.series);
        line.setAttribute("d", pathOf(one.points, toX, toY));
        // No `stroke` and no `fill`. A colour is the application's, and an
        // inline one would be blocked by the CSP the application serves anyway
        // (`CLAUDE.md` §5).
        figure.append(line);

        for (const point of one.points) {
            const mark = svgIn(doc, "circle", classes.point);
            mark.setAttribute("cx", String(toX(point.x)));
            mark.setAttribute("cy", String(toY(point.y)));
            figure.append(mark);

            // A hit region, sized for a finger rather than for the mark. A four
            // pixel target is a target most people miss on a phone.
            const hit = svgIn(doc, "rect", classes.hit);
            hit.setAttribute("x", String(toX(point.x) - 12));
            hit.setAttribute("y", String(toY(point.y) - 12));
            hit.setAttribute("width", "24");
            hit.setAttribute("height", "24");
            hit.setAttribute("fill", "transparent");

            const onPoint = (): void => options.onSelect?.(one, point);
            hit.addEventListener("pointerdown", onPoint);
            hit.addEventListener("pointerover", onPoint);
            closers.add(() => {
                hit.removeEventListener("pointerdown", onPoint);
                hit.removeEventListener("pointerover", onPoint);
            });
            figure.append(hit);
        }
    }

    // The reading. A real table, with a caption and header cells, because that
    // is what a screen reader can navigate by row and column.
    const table = elementIn(doc, "table", classes.table);
    const caption = elementIn(doc, "caption");
    setUserText(caption, copy.caption);
    table.append(caption);

    const head = elementIn(doc, "thead");
    const headRow = elementIn(doc, "tr");
    for (const heading of [copy.seriesHeader, copy.xHeader, copy.yHeader]) {
        const cell = elementIn(doc, "th");
        cell.scope = "col";
        setUserText(cell, heading);
        headRow.append(cell);
    }
    head.append(headRow);
    table.append(head);

    const body = elementIn(doc, "tbody");
    const rows: HTMLTableRowElement[] = [];
    const addresses: { readonly series: ChartSeries; readonly point: ChartPoint }[] = [];

    for (const one of series) {
        for (const point of one.points) {
            const row = elementIn(doc, "tr", classes.row);
            const name = elementIn(doc, "th");
            name.scope = "row";
            setUserText(name, one.label);
            const x = elementIn(doc, "td");
            // The application's formatter. A chart that formatted its own
            // numbers would ship one locale's punctuation to every consumer.
            x.textContent = options.formatX(point.x);
            const y = elementIn(doc, "td");
            y.textContent = options.formatY(point.y);
            row.append(name, x, y);
            body.append(row);
            rows.push(row);
            addresses.push({ series: one, point });
        }
    }
    table.append(body);

    // One tab stop for the whole series, and never a positive `tabindex`. The
    // drawing is hidden, so this is the keyboard path.
    let at = 0;
    const focusRow = (next: number): void => {
        if (rows.length === 0) {
            return;
        }
        at = Math.max(0, Math.min(next, rows.length - 1));
        for (let i = 0; i < rows.length; i += 1) {
            const row = rows[i];
            if (row === undefined) {
                continue;
            }
            row.tabIndex = i === at ? 0 : -1;
            if (i === at) {
                row.classList.add(classes.current);
            } else {
                row.classList.remove(classes.current);
            }
        }
        rows[at]?.focus();
        const address = addresses[at];
        if (address !== undefined) {
            options.onSelect?.(address.series, address.point);
        }
    };

    for (let i = 0; i < rows.length; i += 1) {
        const row = rows[i];
        if (row !== undefined) {
            row.tabIndex = i === 0 ? 0 : -1;
        }
    }

    const onKeyDown = (event: KeyboardEvent): void => {
        const moves: Readonly<Record<string, number>> = {
            ArrowRight: at + 1,
            ArrowDown: at + 1,
            ArrowLeft: at - 1,
            ArrowUp: at - 1,
            Home: 0,
            End: rows.length - 1,
        };
        if (!Object.prototype.hasOwnProperty.call(moves, event.key)) {
            return;
        }
        // The page would otherwise scroll under the figure somebody is reading.
        event.preventDefault();
        focusRow(moves[event.key] ?? at);
    };
    table.addEventListener("keydown", onKeyDown);
    closers.add(() => table.removeEventListener("keydown", onKeyDown));

    // Named for assistive technology, which is what makes it a figure rather
    // than a stray table.
    root.role = "group";
    root.setAttribute("aria-label", isolatedAttribute(copy.caption));

    root.append(figure, table);
    mount.append(root);
    closers.add(() => detach(root));

    return { element: root, close: () => closers.run() };
}
