#pragma once

// Streaming CSV export of one form's submissions.
//
// anvil is a library and holds no routes, so what ships here is the half a
// controller cannot get wrong on its own: the row assembly, the formula-injection
// guard, the BOM, and the paging that keeps memory bounded. The application owns
// the route, the permission in front of it, the audit row behind it, and how the
// finished file reaches the client.
//
// --- three properties, each learned the expensive way ------------------------
//
//   NEVER MATERIALISED. A form with 100 000 submissions must not become one
//   string, and must not become one vector either. Rows are written to a sink in
//   cursor order, one page resident at a time, and the sink is expected to be a
//   file the web server later hands over with sendfile — not a response body
//   assembled in the heap.
//
//   FORMULA INJECTION IS NEUTRALISED. A submitter's answer of
//   `=HYPERLINK("http://evil","click")` executes the moment a staff member opens
//   the export. anvil/http/csv_writer.h prefixes every such cell; this file is
//   what makes sure every ANSWER goes through it.
//
//   THE IDENTITY COLUMN IS OPT-IN AND SEPARATE. `include_identity` is a decision
//   the caller makes after checking a DIFFERENT authority from the one that let
//   them read the submissions at all, and after writing the audit row. The
//   default carries the redacted form, which was computed at seal time and needs
//   no key — so the ordinary export path never touches key material.
//
// --- the column order is the definition's field order ------------------------
//
// Not the answer order, which varies per row: a spreadsheet column has to mean
// one thing all the way down. A field with no answer in a row is an EMPTY cell,
// which is also what an optional field sent blank produces — the two really are
// the same thing here, which is why an empty optional answer is stored as nothing
// rather than as an empty string.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include <mongocxx/client.hpp>

#include "anvil/core/locale.h"
#include "anvil/core/result.h"
#include "anvil/forms/definition.h"
#include "anvil/forms/field_type.h"
#include "anvil/forms/repository.h"

namespace anvil::forms {

// Returns false to stop the export early — a disk that filled, a client that
// disconnected. The chunk is valid only for the duration of the call.
using ExportSink = std::function<bool(std::string_view chunk)>;

// How much is assembled before the sink is called. One buffer, reused, so the
// export's memory is this number plus one page of decoded rows regardless of how
// many submissions the form has.
inline constexpr std::size_t kExportChunkBytes = 64 * 1024;

struct ExportOptions final {
    // Which locale the header labels are written in. Values are never translated
    // — an option's stored value is what an analyst pivots on, and a localised
    // one makes two exports of the same form incomparable.
    Locale locale;
    // Rows per database round trip. Clamped to the repository's page bound.
    std::int32_t page_rows = kMaxSubmissionPageRows;
    // Writes the UNSEALED identity into the identity column instead of the
    // redacted form. See the header comment: this is a separate authority and a
    // separate audit row, and anvil cannot check either one.
    bool include_identity = false;
};

// The header row, preceded by the UTF-8 BOM.
//
// The BOM is what stops Excel decoding the file in the system codepage and
// rendering Arabic as mojibake. The bytes are correct either way; the file is
// unreadable to the people it was exported for.
void append_export_header(std::string& out, const FormDefinition& form, Locale locale);

// One submission as one row. `identity_cell` is written into the identity column
// when the form declares a PII field — the redacted string for an ordinary
// export, the unsealed value for an audited one — and ignored otherwise.
void append_export_row(std::string& out, const FormDefinition& form,
                       const SubmissionRecord& record, std::string_view identity_cell);

// The whole export, streamed.
//
// `unseal` is called once per row when `include_identity` is set, and not at all
// otherwise — so an export that does not carry identities never reaches for a
// key. It returns the empty string when an envelope will not open, which is
// written as an empty cell rather than as an error: one unreadable envelope must
// not cost an operator the other 99 999 rows.
using IdentityUnsealFn = std::function<std::string(const SubmissionRecord&)>;

[[nodiscard]] Status export_submissions(mongocxx::client& client,
                                        const FormRepository& forms,
                                        const FormDefinition& form,
                                        const ExportOptions& options, const ExportSink& sink,
                                        const IdentityUnsealFn& unseal = {});

}  // namespace anvil::forms
