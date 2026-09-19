#!/usr/bin/env node
//
// The generator, as a command. Node only, build time only, and never reachable
// from a browser entry point: it is a path walker and a code emitter, and a
// bundle containing it ships both to every device for no reason
// (`docs/00-architecture.md` §2).
//
// What it prints is a key and an issue, never a sentence about what somebody
// should do. A build-time tool has an audience of one and a locale of one, but
// the discipline is worth keeping anyway: the key is the thing a person opens
// the file at, and prose around it is prose that goes stale where the format
// does not.

import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import process from "node:process";

import type { DescriptorProblem } from "./descriptor.js";
import { readDescriptorJson } from "./descriptor.js";
import { emitClient, kOutputFileName } from "./emit.js";

const kUsage = "usage: hammer codegen --descriptor <path> --out <dir>";

type Invocation = {
    readonly descriptorPath: string;
    readonly outDirectory: string;
};

function parse(argv: readonly string[]): Invocation | null {
    if (argv[0] !== "codegen") {
        return null;
    }

    let descriptorPath = "";
    let outDirectory = "";

    for (let i = 1; i < argv.length; i += 1) {
        const flag = argv[i];
        const value = argv[i + 1];
        if (value === undefined || value.startsWith("--")) {
            return null;
        }
        if (flag === "--descriptor") {
            descriptorPath = value;
        } else if (flag === "--out") {
            outDirectory = value;
        } else {
            return null;
        }
        i += 1;
    }

    if (descriptorPath.length === 0 || outDirectory.length === 0) {
        return null;
    }
    return { descriptorPath, outDirectory };
}

function report(label: string, problems: readonly DescriptorProblem[]): void {
    for (const problem of problems) {
        const at = problem.key.length === 0 ? "<root>" : problem.key;
        process.stderr.write(`${label} ${at}: ${problem.issue}\n`);
    }
}

function main(argv: readonly string[]): number {
    const invocation = parse(argv);
    if (invocation === null) {
        process.stderr.write(`${kUsage}\n`);
        return 1;
    }

    let text: string;
    try {
        text = readFileSync(invocation.descriptorPath, "utf8");
    } catch {
        process.stderr.write(`error ${invocation.descriptorPath}: unreadable\n`);
        return 1;
    }

    const reading = readDescriptorJson(text);
    if (!reading.ok) {
        report("error", reading.error);
        process.stderr.write(`error ${invocation.descriptorPath}: not generated\n`);
        return 1;
    }

    // A dead entry reads as coverage, so it is reported. It is not a failure:
    // anvil limits by keys that are not route buckets and mints scopes for
    // operations whose routes its emitter does not carry, so failing here would
    // reject anvil's own output.
    report("warning", reading.value.warnings);

    const target = join(invocation.outDirectory, kOutputFileName);
    try {
        mkdirSync(invocation.outDirectory, { recursive: true });
        writeFileSync(target, emitClient(reading.value.descriptor), "utf8");
    } catch {
        process.stderr.write(`error ${target}: unwritable\n`);
        return 1;
    }

    process.stdout.write(`${target}\n`);
    return 0;
}

process.exitCode = main(process.argv.slice(2));
