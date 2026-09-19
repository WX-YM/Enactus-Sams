// The `hammer/react` entry point: the stores, bound to one framework's
// lifecycle, and nothing else.
//
// Exports are named and explicit, for the reason every other entry point gives:
// a star re-export puts every module in every bundle that touches any of them.
//
// There is one module behind this and there is expected to be one. A second
// adapter is written against the same store surface when a consumer needs it
// (`docs/15-tasks.md` §Deferred), and its existence is the proof the boundary
// held rather than a reason to widen this one.

export type { Mutation, MutationState, ResourceView } from "./hooks.js";
export {
    useForm,
    useInbox,
    useMutation,
    useResource,
    useSession,
    useStore,
} from "./hooks.js";
