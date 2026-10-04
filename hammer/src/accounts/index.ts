// The `hammer/accounts` entry point: the client of anvil's built-in account
// flows — registration, verification, sign-in, reset, change and sign-out —
// driven from the `kAccounts` table the descriptor publishes
// (`docs/01-seams.md` §23).
//
// Its own entry point for the reason `hammer/prehash` is: only the screens
// that sign somebody in need it.

export type {
    AccountCall,
    AccountFailure,
    AccountFlow,
    AccountRole,
    AccountsConfig,
    AccountsSpec,
    IdentifierKind,
    RegisterInput,
} from "./accounts.js";
export { Accounts, accountCall } from "./accounts.js";

export { accountFields } from "./fields.js";
