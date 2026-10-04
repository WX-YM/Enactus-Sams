// The `hammer/edit` entry point: crop, rotate, flip, resize and freehand
// drawing on a stored image, as a canonical recipe anvil renders
// (`docs/04-image-edits.md`).
//
// Its own entry point for the reason `hammer/chart` is: most applications never
// show an image editor, and they should pay nothing for one (`CLAUDE.md` §2.1).

export type {
    EditLimits,
    EditPlan,
    EncodedRecipe,
    FixedRect,
    PixelBox,
    QuarterTurns,
    Recipe,
    RecipeError,
    RecipeFault,
    SourceSize,
    Stroke,
} from "./recipe.js";
export { decodeRecipe, encodeRecipe, kEmptyRecipe, kFaultWire, kFixedOne, kMaxStrokeWidth, planEdit } from "./recipe.js";

export type { CropAspect, CropHandle } from "./geometry.js";
export { mirror, rotate } from "./geometry.js";

export { EditHistory, kHistoryEntries } from "./history.js";

export type {
    EditAnswerError,
    EditCall,
    EditedMedia,
    MediaSubject,
    ReopenEditOptions,
    ReopenedEdit,
    SubmitEditOptions,
} from "./submit.js";
export { editCall, reopenEdit, submitEdit } from "./submit.js";

export type {
    EditorColour,
    EditorCopy,
    EditorHandle,
    EditorOptions,
    EditorPart,
    EditorRefusal,
    EditorTool,
    EditorWidth,
} from "./editor.js";
export { renderImageEditor } from "./editor.js";
