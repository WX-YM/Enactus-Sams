// A seeded generator, so a property test that fails fails again.
//
// `Math.random` would make every one of these tests a different test on every
// run: a failure that cannot be reproduced is a failure that gets re-run until
// it passes and then forgotten. The seed is printed with the assertion, so a red
// build names the exact input that broke it.
//
// xorshift32: four operations, no dependency, and more than enough distribution
// for choosing indices out of a table of code points.

export class Prng {
    private state: number;

    constructor(seed: number) {
        // Zero is the one state xorshift cannot leave.
        this.state = seed === 0 ? 0x9e3779b9 : seed >>> 0;
    }

    next(): number {
        let x = this.state;
        x ^= x << 13;
        x ^= x >>> 17;
        x ^= x << 5;
        this.state = x >>> 0;
        return this.state;
    }

    // [0, bound)
    below(bound: number): number {
        return this.next() % bound;
    }

    pick<T>(values: readonly T[]): T {
        const chosen = values[this.below(values.length)];
        if (chosen === undefined) {
            throw new Error("pick from an empty table");
        }
        return chosen;
    }
}
