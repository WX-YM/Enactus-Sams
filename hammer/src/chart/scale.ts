// Scales and ticks. This is arithmetic, which is why there is no dependency.
//
// A charting library is thirty to ninety kilobytes of somebody else's opinions
// about colour, and the part of it that is load-bearing is a linear map from one
// interval to another plus a routine for choosing round numbers. Both are below,
// and the zero-dependency rule has no exception for convenience
// (`CLAUDE.md` §9).

export type Interval = {
    readonly from: number;
    readonly to: number;
};

// Maps a value in one interval onto another.
//
// A degenerate domain — every point identical, one point, no points — maps to
// the middle of the range rather than dividing by zero. A flat series is an
// ordinary thing for a metric to be, and a chart of one should draw a flat line
// rather than `NaN` for every coordinate, which renders as nothing at all with
// no error anywhere.
export function linear(domain: Interval, range: Interval): (value: number) => number {
    const span = domain.to - domain.from;
    if (span === 0) {
        const middle = (range.from + range.to) / 2;
        return () => middle;
    }
    const scale = (range.to - range.from) / span;
    return (value) => range.from + (value - domain.from) * scale;
}

// The interval a set of values spans, or null for no values at all.
export function extent(values: Iterable<number>): Interval | null {
    let from = Number.POSITIVE_INFINITY;
    let to = Number.NEGATIVE_INFINITY;
    let seen = false;

    // One pass, no intermediate arrays: a chart of ten thousand points would
    // otherwise allocate three of them for a minimum and a maximum
    // (`CLAUDE.md` §2.2).
    for (const value of values) {
        if (!Number.isFinite(value)) {
            continue;
        }
        seen = true;
        if (value < from) {
            from = value;
        }
        if (value > to) {
            to = value;
        }
    }

    return seen ? { from, to } : null;
}

// Round numbers inside an interval, at most about `count` of them.
//
// The step is a power of ten times 1, 2, 5 or 10, which is the standard answer
// and the reason is perceptual rather than mathematical: those are the intervals
// people read without doing arithmetic. A step of 3.7 divides an axis correctly
// and unreadably.
//
// The thresholds are the square roots — √2, √10, √50 — rather than the bare
// 1, 2, 5. They are the geometric midpoints between one candidate step and the
// next, so each step is chosen over its neighbour when it is genuinely closer in
// ratio. Using the bare numbers biases every decision upward and produces an
// axis with three labels where somebody asked for five.
const kSqrt2 = Math.SQRT2;
const kSqrt10 = Math.sqrt(10);
const kSqrt50 = Math.sqrt(50);

export function ticks(domain: Interval, count: number): readonly number[] {
    if (count < 1 || !Number.isFinite(domain.from) || !Number.isFinite(domain.to)) {
        return [];
    }
    if (domain.from === domain.to) {
        return [domain.from];
    }

    const low = Math.min(domain.from, domain.to);
    const high = Math.max(domain.from, domain.to);
    const rough = (high - low) / count;
    const magnitude = 10 ** Math.floor(Math.log10(rough));
    const ratio = rough / magnitude;

    const step =
        magnitude * (ratio >= kSqrt50 ? 10 : ratio >= kSqrt10 ? 5 : ratio >= kSqrt2 ? 2 : 1);

    // How many decimals the step itself has. Every tick is rounded to exactly
    // that, and the rounding is the point: 0.1 is not representable in binary,
    // so adding it ten times produces 0.30000000000000004 — which is a correct
    // float and an axis label somebody has to read.
    const decimals = Math.max(0, -Math.floor(Math.log10(step)));

    const out: number[] = [];
    const first = Math.ceil(low / step) * step;
    // Counted rather than accumulated. `at += step` compounds the error at every
    // tick; multiplying a whole number by the step commits it once.
    for (let i = 0; ; i += 1) {
        const value = Number((first + i * step).toFixed(decimals));
        if (value > high + step / 1e9) {
            break;
        }
        out.push(value);
    }
    return out;
}
