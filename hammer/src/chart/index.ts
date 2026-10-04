// The `hammer/chart` entry point: scales, marks, hit regions, a keyboard path
// and the data table that is the figure's actual reading.
//
// Its own entry point so an application with no dashboard pays nothing for one,
// and it may import `hammer` and `hammer/state` but not `hammer/dom` — a chart
// drives a store and does not need the insertion site, and reaching across would
// put the form renderer and the bell in every bundle that draws a graph.
//
// No charting dependency. A linear scale and a path string are arithmetic, and
// the zero-dependency rule has no exception for convenience (`CLAUDE.md` §9).

export type { Mounted } from "./mount.js";

export type { Interval } from "./scale.js";
export { extent, linear, ticks } from "./scale.js";

export type { ChartCopy, ChartOptions, ChartPart, ChartPoint, ChartSeries } from "./chart.js";
export { renderChart } from "./chart.js";
